/*
 * transaction.c (Linux Shared-Memory Transaction Engine)
 *
 * Executes deposits, withdrawals, transfers, balance lookups, status polls,
 * and operator deposit approvals across multiple forked worker processes.
 *
 * Why everything lives inside ctx->bank (System V Shared Memory):
 * Because Linux workers are separate processes spawned via fork(), normal
 * static/global variables would give each worker its own isolated copy.
 * Keeping both the pending-deposit queue and the 128-entry idempotency cache
 * directly inside shared memory ensures every worker and the parent operator
 * thread always see the exact same state.
 */

#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <inttypes.h>  /* PRId64 portable 64-bit printf format specifier */
#include <sched.h>     /* sched_yield() used in --nolock race demonstrations */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>      /* time(), nanosleep(), struct timespec */
#include <unistd.h>    /* getpid() */

/*
 * Builds a unique transaction ID per operation without needing a shared counter lock.
 * Combining worker_id (0 in parent, 1..N in workers), the OS process ID, and a
 * per-process static sequence counter guarantees zero collisions across forked
 * workers and server restarts.
 */
static void next_transaction_id(const server_ctx_t *ctx, transaction_id_t *id) {
    static unsigned long counter = 0;

    memset(id, 0, sizeof(*id));
    snprintf(id->value, sizeof(id->value), "T%02d-%ld-%lu",
             ctx->worker_id, (long)getpid(), ++counter);
}

/*
 * Reads, then writes account balance.
 * With the semaphore lock held, atomicity is guaranteed.
 * When running with --nolock, sched_yield() and a 50-microsecond nanosleep()
 * force the kernel to context-switch between read and write, causing concurrent
 * workers to overwrite each other's updates and demonstrate a lost update.
 */
static void set_balance(const server_ctx_t *ctx, account_t *account, money_t change) {
    money_t current = account->balance;

    if (!ctx->use_lock) {
        sched_yield();
        struct timespec delay_ts;
        delay_ts.tv_sec  = 0;
        delay_ts.tv_nsec = 50000L;  /* 50 microseconds */
        nanosleep(&delay_ts, NULL);
    }

    account->balance = current + change;
    account->version++;
}

/*
 * Check for signed 64-bit integer overflow *before* adding (account->balance > INT64_MAX - amount)
 * because signed overflow in C is undefined behavior and can wrap negative.
 */
static status_t do_deposit(const server_ctx_t *ctx, account_t *account, money_t amount) {
    if (account->balance > INT64_MAX - amount) {
        return STATUS_BALANCE_OVERFLOW;
    }

    set_balance(ctx, account, amount);
    account->total_deposits += amount;
    return STATUS_OK;
}

/* Reject withdrawal if the account lacks enough funds so balances never go negative */
static status_t do_withdrawal(const server_ctx_t *ctx, account_t *account, money_t amount) {
    if (account->balance < amount) {
        return STATUS_INSUFFICIENT_FUNDS;
    }

    set_balance(ctx, account, -amount);
    account->total_withdrawals += amount;
    return STATUS_OK;
}

/*
 * Validate all preconditions (self-transfer, sender funds, receiver overflow) up
 * front so we never end up in a half-baked state where money leaves 'from' but
 * fails to land in 'to'.
 */
static status_t do_transfer(const server_ctx_t *ctx, account_t *from, account_t *to, money_t amount) {
    if (from == to) {
        return STATUS_SELF_TRANSFER;
    }
    if (from->balance < amount) {
        return STATUS_INSUFFICIENT_FUNDS;
    }
    if (to->balance > INT64_MAX - amount) {
        return STATUS_BALANCE_OVERFLOW;
    }

    set_balance(ctx, from, -amount);
    set_balance(ctx, to, amount);
    return STATUS_OK;
}

/*
 * Stores a completed or rejected request outcome in the shared circular buffer
 * (modulo IDEMPOTENCY_CACHE_SIZE) so client retries with the same request_id
 * return the cached outcome instead of double-executing. Request ID 0 is skipped
 * because uninitialized or raw test packets often leave it at zero.
 *
 * Caller must hold ctx->lock before touching shared bank->idempotency_cache.
 */
static void record_idempotency_locked(bank_t *bank, uint32_t req_id, const client_id_t *client_id, int32_t op,
                                      status_t st, money_t balance, const char *txn_id) {
    if (bank == NULL || req_id == 0) {
        return;
    }

    int idx = bank->idempotency_idx % IDEMPOTENCY_CACHE_SIZE;
    bank->idempotency_cache[idx].request_id    = req_id;
    bank->idempotency_cache[idx].client_id     = *client_id;
    bank->idempotency_cache[idx].operation     = op;
    bank->idempotency_cache[idx].status        = st;
    bank->idempotency_cache[idx].final_balance = balance;

    strncpy(bank->idempotency_cache[idx].txn_id, txn_id ? txn_id : "", MAX_TRANSACTION_ID);
    bank->idempotency_cache[idx].txn_id[MAX_TRANSACTION_ID] = '\0';
    bank->idempotency_cache[idx].timestamp = (uint64_t)time(NULL);

    bank->idempotency_idx++;
}

/*
 * Searches the shared idempotency ring buffer by both request_id and client_id,
 * since different clients maintain independent request counters and could easily
 * pick the same request_id.
 *
 * Caller must hold ctx->lock before reading shared bank->idempotency_cache.
 */
static int find_idempotency_locked(const bank_t *bank, uint32_t req_id, const client_id_t *client_id, idempotency_entry_t *out) {
    if (bank == NULL || req_id == 0) {
        return 0;
    }

    for (int i = 0; i < IDEMPOTENCY_CACHE_SIZE; i++) {
        if (bank->idempotency_cache[i].request_id == req_id &&
            strncmp(bank->idempotency_cache[i].client_id.value, client_id->value, CLIENT_ID_LENGTH) == 0) {
            if (out != NULL) {
                *out = bank->idempotency_cache[i];
            }
            return 1;
        }
    }

    return 0;
}

/*
 * Enqueues a deposit into shared memory so the parent operator thread sees it.
 * Notice how we copy the newly added slot into a local 'snapshot' struct before
 * releasing the semaphore—that way we don't hold the global IPC lock during slow
 * printf() calls, and we avoid reading shared memory unlocked while printing.
 */
static status_t enqueue_pending_deposit(server_ctx_t *ctx, const Message *request, const char *txn_id) {
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    if (ctx->bank->pending_count >= MAX_PENDING_DEPOSITS) {
        if (ctx->use_lock) {
            semaphore_unlock(ctx->lock);
        }
        return STATUS_QUEUE_FULL;
    }

    pending_deposit_t *p = &ctx->bank->pending[ctx->bank->pending_count++];
    p->request_id = request->request_id;
    p->client_pid = request->sender_pid;

    snprintf(p->client_id.value, sizeof(p->client_id.value), "%.*s", (int)CLIENT_ID_LENGTH, request->client_id);

    p->amount = request->amount;

    snprintf(p->txn_id, sizeof(p->txn_id), "%.*s", (int)MAX_TRANSACTION_ID, txn_id);

    p->active     = 1;
    p->created_at = (uint64_t)time(NULL);

    pending_deposit_t snapshot = *p;

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    printf("\n[OPERATOR ALERT] New Pending Cash Deposit:\n");
    printf("  Transaction ID: %s\n", snapshot.txn_id);
    printf("  Client ID:      %s\n", snapshot.client_id.value);
    printf("  Amount:         %" PRId64 " %s\n", snapshot.amount, CURRENCY_CODE);
    printf("  Type 'approve %s' or 'reject %s' to process.\n\n", snapshot.txn_id, snapshot.txn_id);
    fflush(stdout);

    return STATUS_TRANSACTION_PENDING;
}

/*
 * Called by the operator thread in the parent process to approve a queued deposit.
 * Credits the account, marks the slot inactive, and caches the final balance in
 * the shared idempotency table—all inside one atomic semaphore section—then
 * releases the lock before writing to the WAL file or printing to stdout.
 */
status_t transaction_approve_deposit(server_ctx_t *ctx, const char *txn_id) {
    if (ctx == NULL || txn_id == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    pending_deposit_t *target = NULL;
    for (int i = 0; i < ctx->bank->pending_count; i++) {
        if (ctx->bank->pending[i].active && strcmp(ctx->bank->pending[i].txn_id, txn_id) == 0) {
            target = &ctx->bank->pending[i];
            break;
        }
    }

    if (target == NULL) {
        if (ctx->use_lock) {
            semaphore_unlock(ctx->lock);
        }
        return STATUS_NOT_FOUND;
    }

    account_t *account = registry_find(ctx->bank, &target->client_id);
    status_t result    = STATUS_NOT_FOUND;
    money_t new_bal    = 0;

    if (account != NULL) {
        result  = do_deposit(ctx, account, target->amount);
        new_bal = account->balance;
    }

    /* Mark inactive and copy locally before unlocking so post-lock logging stays safe */
    target->active = 0;
    pending_deposit_t saved = *target;

    record_idempotency_locked(ctx->bank, saved.request_id, &saved.client_id, MSG_DEPOSIT, result, new_bal, saved.txn_id);

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    /* Log committed transaction to the Write-Ahead Log outside the critical section */
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    snprintf(tx.transaction_id.value, sizeof(tx.transaction_id.value), "%.*s", (int)MAX_TRANSACTION_ID, saved.txn_id);
    snprintf(tx.request_id.value, sizeof(tx.request_id.value), "%u", saved.request_id);
    tx.type         = TRANSACTION_DEPOSIT;
    tx.sender_id    = saved.client_id;
    tx.amount       = saved.amount;
    tx.state        = (result == STATUS_OK) ? TRANSACTION_COMMITTED : TRANSACTION_FAILED;
    tx.result       = result;
    tx.completed_at = (uint64_t)time(NULL);
    persistence_log_transaction(&tx);
    printf("Deposit %s approved for %s: %" PRId64 " %s (new balance: %" PRId64 ")\n",
           saved.txn_id, saved.client_id.value, saved.amount, CURRENCY_CODE, new_bal);
    fflush(stdout);

    return result;
}

/*
 * Marks a queued deposit as rejected in shared memory without modifying any
 * account balance, while caching STATUS_TRANSACTION_REJECTED so any worker
 * handling a status poll from the client sees the rejection immediately.
 */
status_t transaction_reject_deposit(server_ctx_t *ctx, const char *txn_id) {
    if (ctx == NULL || txn_id == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    /* Scan active pending slots in shared memory for a matching transaction ID */
    pending_deposit_t *target = NULL;
    for (int i = 0; i < ctx->bank->pending_count; i++) {
        if (ctx->bank->pending[i].active && strcmp(ctx->bank->pending[i].txn_id, txn_id) == 0) {
            target = &ctx->bank->pending[i];
            break;
        }
    }

    if (target == NULL) {
        if (ctx->use_lock) {
            semaphore_unlock(ctx->lock);
        }
        return STATUS_NOT_FOUND;
    }

    /* Mark inactive and take a local copy before releasing the lock for disk/console I/O */
    target->active = 0;
    pending_deposit_t saved = *target;

    record_idempotency_locked(ctx->bank, saved.request_id, &saved.client_id, MSG_DEPOSIT,
                              STATUS_TRANSACTION_REJECTED, 0, saved.txn_id);

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    /* Append rejection record to the Write-Ahead Log outside the critical section */
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    snprintf(tx.transaction_id.value, sizeof(tx.transaction_id.value), "%.*s", (int)MAX_TRANSACTION_ID, saved.txn_id);
    snprintf(tx.request_id.value, sizeof(tx.request_id.value), "%u", saved.request_id);
    tx.type         = TRANSACTION_DEPOSIT;
    tx.sender_id    = saved.client_id;
    tx.amount       = saved.amount;
    tx.state        = TRANSACTION_REJECTED;
    tx.result       = STATUS_TRANSACTION_REJECTED;
    tx.completed_at = (uint64_t)time(NULL);
    persistence_log_transaction(&tx);

    printf("Deposit %s rejected for %s\n", txn_id, saved.client_id.value);
    fflush(stdout);
    return STATUS_OK;
}

/*
 * Copies active pending deposits out of shared memory into a caller-supplied
 * buffer under the lock so the operator CLI can print a consistent snapshot.
 */
int transaction_get_pending(server_ctx_t *ctx, pending_deposit_t *list, int max_count) {
    if (ctx == NULL || ctx->bank == NULL || list == NULL || max_count <= 0) {
        return 0;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < ctx->bank->pending_count && count < max_count; i++) {
        if (ctx->bank->pending[i].active) {
            list[count++] = ctx->bank->pending[i];
        }
    }

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    return count;
}

/*
 * Lets a client poll for the outcome of an earlier request_id (for example,
 * after Ctrl+C interrupts an in-flight call or while waiting for a pending
 * cash deposit to be approved).
 */
status_t transaction_status_query(server_ctx_t *ctx, uint32_t request_id, Message *response) {
    if (ctx == NULL || response == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* Normalize response->client_id into a fixed-size, null-terminated lookup key */
    client_id_t cid;
    memset(&cid, 0, sizeof(cid));
    snprintf(cid.value, sizeof(cid.value), "%.*s", (int)CLIENT_ID_LENGTH, response->client_id);

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    idempotency_entry_t entry;
    int found = find_idempotency_locked(ctx->bank, request_id, &cid, &entry);

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    if (found) {
        /* Populate response with the historical transaction's status, balance, and TxnID */
        response->status_code = entry.status;
        response->amount      = entry.final_balance;
        strncpy(response->txn_id, entry.txn_id, TXN_ID_LEN - 1);
        response->txn_id[TXN_ID_LEN - 1] = '\0';
        return STATUS_OK;
    }

    response->status_code = STATUS_NOT_FOUND;
    return STATUS_NOT_FOUND;
}

/*
 * Main entry point for financial mutations (deposit, withdrawal, transfer).
 * Flow:
 *   1. Check shared idempotency cache -> return cached reply if already processed.
 *   2. Generate a fresh transaction ID and validate amount > 0.
 *   3. Queue cash deposits for operator approval unless --auto-approve is active.
 *   4. Acquire global semaphore -> execute balance update -> record idempotency -> unlock.
 *   5. Append outcome to the Write-Ahead Log outside the lock.
 */
status_t transaction_execute(server_ctx_t *ctx, const Message *request, Message *response) {
    if (ctx == NULL || request == NULL || response == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* Normalize untrusted wire strings into null-terminated fixed structs */
    client_id_t sender_id, receiver_id;
    memset(&sender_id, 0, sizeof(sender_id));
    memset(&receiver_id, 0, sizeof(receiver_id));
    snprintf(sender_id.value, sizeof(sender_id.value), "%.*s", (int)CLIENT_ID_LENGTH, request->client_id);
    snprintf(receiver_id.value, sizeof(receiver_id.value), "%.*s", (int)CLIENT_ID_LENGTH, request->receiver_id);

    /* 1. Idempotency check under lock: short-circuit immediately if we already ran this request_id */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        response->status_code = STATUS_SYNCHRONIZATION_ERROR;
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    idempotency_entry_t cached;
    int already_done = find_idempotency_locked(ctx->bank, request->request_id, &sender_id, &cached);

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    if (already_done) {
        response->status_code = cached.status;
        response->amount      = cached.final_balance;
        strncpy(response->txn_id, cached.txn_id, TXN_ID_LEN - 1);
        response->txn_id[TXN_ID_LEN - 1] = '\0';
        return cached.status;
    }

    /* 2. Generate a unique transaction ID and copy it onto the outgoing response */
    transaction_id_t txn_id;
    next_transaction_id(ctx, &txn_id);
    strncpy(response->txn_id, txn_id.value, TXN_ID_LEN - 1);
    response->txn_id[TXN_ID_LEN - 1] = '\0';

    /* Reject zero or negative amounts up front and cache the rejection */
    if (request->amount <= 0) {
        response->status_code = STATUS_INVALID_AMOUNT;
        if (ctx->use_lock && semaphore_lock(ctx->lock) == STATUS_OK) {
            record_idempotency_locked(ctx->bank, request->request_id, &sender_id, request->operation,
                                      STATUS_INVALID_AMOUNT, 0, txn_id.value);
            semaphore_unlock(ctx->lock);
        }
        return STATUS_INVALID_AMOUNT;
    }

    /* 3. Operator approval gate for cash deposits (bypassed when --auto-approve is set) */
    if (request->operation == MSG_DEPOSIT && !ctx->auto_approve) {
        status_t st = enqueue_pending_deposit(ctx, request, txn_id.value);
        response->status_code = st;
        return st;
    }

    /* 4. Critical Section: acquire global semaphore before reading/modifying account balances */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        response->status_code = STATUS_SYNCHRONIZATION_ERROR;
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    status_t result   = STATUS_ERROR;
    account_t *sender = registry_find(ctx->bank, &sender_id);
    money_t final_bal = 0;

    if (sender == NULL) {
        result = STATUS_ACCOUNT_NOT_FOUND;
    } else if (request->operation == MSG_DEPOSIT) {
        result    = do_deposit(ctx, sender, request->amount);
        final_bal = sender->balance;
    } else if (request->operation == MSG_WITHDRAW) {
        result    = do_withdrawal(ctx, sender, request->amount);
        final_bal = sender->balance;
    } else if (request->operation == MSG_TRANSFER) {
        account_t *receiver = registry_find(ctx->bank, &receiver_id);
        if (receiver == NULL) {
            result = STATUS_ACCOUNT_NOT_FOUND;
        } else {
            result    = do_transfer(ctx, sender, receiver, request->amount);
            final_bal = sender->balance;
        }
    } else {
        result = STATUS_INVALID_ARGUMENT;
    }

    if (sender != NULL) {
        response->amount = final_bal;
    }

    /* Cache outcome while still holding the lock so concurrent retries see it atomically */
    record_idempotency_locked(ctx->bank, request->request_id, &sender_id, request->operation, result, final_bal, txn_id.value);

    /* Release semaphore lock before doing slower disk I/O */
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    response->status_code = result;

    /* 5. Write-Ahead Log: record both committed and rejected attempts for auditing */
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.transaction_id = txn_id;
    snprintf(tx.request_id.value, sizeof(tx.request_id.value), "%u", request->request_id);

    if (request->operation == MSG_DEPOSIT) {
        tx.type = TRANSACTION_DEPOSIT;
    } else if (request->operation == MSG_WITHDRAW) {
        tx.type = TRANSACTION_WITHDRAWAL;
    } else if (request->operation == MSG_TRANSFER) {
        tx.type = TRANSACTION_TRANSFER;
    }

    tx.sender_id    = sender_id;
    tx.receiver_id  = receiver_id;
    tx.amount       = request->amount;
    tx.state        = (result == STATUS_OK) ? TRANSACTION_COMMITTED : TRANSACTION_REJECTED;
    tx.result       = result;
    tx.completed_at = (uint64_t)time(NULL);
    persistence_log_transaction(&tx);

    return result;
}

/*
 * Reads an account's current balance and version from shared memory under the
 * global semaphore lock. Holding the lock during a read prevents returning a
 * torn or mid-transfer balance while another worker is in the middle of debiting
 * one account and crediting another.
 */
status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response) {
    /* Guard all pointers up front before touching shared memory or zeroing output */
    if (ctx == NULL || id == NULL || response == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* Wipe the caller's output struct first so no uninitialized stack bytes leak if lookup fails */
    memset(response, 0, sizeof(*response));

    /* Acquire the System V semaphore unless the server was launched with --nolock */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    status_t result    = STATUS_ACCOUNT_NOT_FOUND;
    account_t *account = registry_find(ctx->bank, id);

    if (account != NULL) {
        /* Copy the client ID, live balance, and monotonic version counter atomically */
        response->client_id       = account->client_id;
        response->balance         = account->balance;
        response->account_version = account->version;
        result                    = STATUS_OK;
    }

    /* Always release the semaphore before returning so other workers aren't starved */
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    return result;
}

