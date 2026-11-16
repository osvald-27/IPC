/*
 * transaction.c (Linux Shared-Memory Transaction Engine)
 *
 * Executes deposits, withdrawals, transfers, balance lookups, and operator
 * deposit approvals across multiple forked worker processes.
 *
 * Key architectural note:
 * Because Linux workers are separate processes spawned via fork(), regular
 * static/global variables would give each worker its own isolated copy.
 * Keeping both the pending-deposit queue and the 128-entry idempotency cache
 * directly inside shared memory (ctx->bank) ensures every worker and the
 * parent operator thread always see the exact same state.
 */

#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Builds a unique transaction ID per operation without needing a shared counter lock.
 * Combining worker_id (0 in parent, 1..N in workers), the OS process ID, and a
 * per-process sequence number guarantees zero collisions across forked workers
 * and server restarts.
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
 * When running with --nolock, sched_yield() and a microsecond delay
 * give up the CPU between read and write, causing concurrent workers
 * to overwrite each other's updates and demonstrate the lost update.
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

/* Check for overflow *before* adding so we don't trigger signed int64 undefined behavior */
static status_t do_deposit(const server_ctx_t *ctx, account_t *account, money_t amount) {
    if (account->balance > INT64_MAX - amount) {
        return STATUS_BALANCE_OVERFLOW;
    }

    set_balance(ctx, account, amount);
    account->total_deposits += amount;
    return STATUS_OK;
}

static status_t do_withdrawal(const server_ctx_t *ctx, account_t *account, money_t amount) {
    if (account->balance < amount) {
        return STATUS_INSUFFICIENT_FUNDS;
    }

    set_balance(ctx, account, -amount);
    account->total_withdrawals += amount;
    return STATUS_OK;
}

/*
 * Validate all preconditions (self-transfer, funds, destination overflow) up front
 * so we never end up in a half-baked state where money leaves 'from' but fails
 * to land in 'to'.
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
 * Records a completed or rejected request outcome in the shared circular buffer
 * so retries with the same request_id return the cached outcome instead of
 * double-executing. Request ID 0 is skipped since uninitialized test packets
 * often leave it at zero.
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
 * since different clients maintain independent request counters and could pick
 * the same request_id.
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
            if (out) {
                *out = bank->idempotency_cache[i];
            }
            return 1;
        }
    }

    return 0;
}

/*
 * Enqueues a deposit into shared memory so the parent operator thread sees it.
 * Notice how we copy the newly added slot into a local 'snapshot' before releasing
 * the semaphore—that way we don't hold the global lock during slow printf() calls,
 * and we avoid reading shared memory unlocked while printing.
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

    strncpy(p->client_id.value, request->client_id, CLIENT_ID_LENGTH);
    p->client_id.value[CLIENT_ID_LENGTH] = '\0';

    p->amount = request->amount;

    strncpy(p->txn_id, txn_id, MAX_TRANSACTION_ID);
    p->txn_id[MAX_TRANSACTION_ID] = '\0';

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

    target->active = 0;
    pending_deposit_t saved = *target;

    record_idempotency_locked(ctx->bank, saved.request_id, &saved.client_id, MSG_DEPOSIT, result, new_bal, saved.txn_id);

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    /* Log committed transaction to WAL outside the critical section */
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    strncpy(tx.transaction_id.value, saved.txn_id, MAX_TRANSACTION_ID);
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
target->active = 0;
pending_deposit_t saved = *target;
record_idempotency_locked(ctx->bank, saved.request_id, &saved.client_id, MSG_DEPOSIT,
STATUS_TRANSACTION_REJECTED, 0, saved.txn_id);
if (ctx->use_lock) {
semaphore_unlock(ctx->lock);
}
transaction_t tx;
memset(&tx, 0, sizeof(tx));
strncpy(tx.transaction_id.value, saved.txn_id, MAX_TRANSACTION_ID);
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
• Copies active pending deposits out of shared memory into a caller-supplied
• buffer under the lock so the operator CLI can print a consistent list.
*/
int transaction_get_pending(server_ctx_t *ctx, pending_deposit_t *list, int max_count) {
if (ctx == NULL || ctx->bank == NULL || list == NULL || max_count <= 0) {
return 0;
}if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
return 0;
}int count = 0;
for (int i = 0; i < ctx->bank->pending_count && count < max_count; i++) {
if (ctx->bank->pending[i].active) {
list[count++] = ctx->bank->pending[i];
}
}if (ctx->use_lock) {
semaphore_unlock(ctx->lock);
}return count;
}
/*
• Allows a client to poll for the final status of a previous request_id
• (for example, checking whether a pending cash deposit has been approved).
*/
status_t transaction_status_query(server_ctx_t *ctx, uint32_t request_id, Message *response) {
if (ctx == NULL || response == NULL) {
return STATUS_INVALID_ARGUMENT;
}client_id_t cid;
memset(&cid, 0, sizeof(cid));
strncpy(cid.value, response->client_id, CLIENT_ID_LENGTH);if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
return STATUS_SYNCHRONIZATION_ERROR;
}

