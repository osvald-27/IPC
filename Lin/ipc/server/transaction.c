#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <inttypes.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* In-memory pending deposits queue for operator review */
static pending_deposit_t g_pending[MAX_PENDING_DEPOSITS];
static int g_pending_count = 0;

/* Cache of recent requests to guarantee idempotency */
#define IDEMPOTENCY_CACHE_SIZE 128
typedef struct {
    uint32_t request_id;
    client_id_t client_id;
    int32_t operation;
    status_t status;
    money_t final_balance;
    char txn_id[MAX_TRANSACTION_ID + 1U];
    uint64_t timestamp;
} idempotency_entry_t;

static idempotency_entry_t g_idempotency_cache[IDEMPOTENCY_CACHE_SIZE];
static int g_idempotency_idx = 0;

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
        delay_ts.tv_sec = 0;
        delay_ts.tv_nsec = 50000L;  /* 50 microseconds */
        nanosleep(&delay_ts, NULL);
    }
    account->balance = current + change;
    account->version++;
}

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

static void record_idempotency(uint32_t req_id, const client_id_t *client_id, int32_t op,
                               status_t st, money_t balance, const char *txn_id) {
    if (req_id == 0) return;
    int idx = g_idempotency_idx % IDEMPOTENCY_CACHE_SIZE;
    g_idempotency_cache[idx].request_id = req_id;
    g_idempotency_cache[idx].client_id = *client_id;
    g_idempotency_cache[idx].operation = op;
    g_idempotency_cache[idx].status = st;
    g_idempotency_cache[idx].final_balance = balance;
    strncpy(g_idempotency_cache[idx].txn_id, txn_id ? txn_id : "", MAX_TRANSACTION_ID);
    g_idempotency_cache[idx].timestamp = (uint64_t)time(NULL);
    g_idempotency_idx++;
}

static int find_idempotency(uint32_t req_id, const client_id_t *client_id, idempotency_entry_t *out) {
    if (req_id == 0) return 0;
    for (int i = 0; i < IDEMPOTENCY_CACHE_SIZE; i++) {
        if (g_idempotency_cache[i].request_id == req_id &&
            strncmp(g_idempotency_cache[i].client_id.value, client_id->value, CLIENT_ID_LENGTH) == 0) {
            if (out) *out = g_idempotency_cache[i];
            return 1;
        }
    }
    return 0;
}

/* Enqueues a deposit for operator review */
static status_t enqueue_pending_deposit(const server_ctx_t *ctx, const Message *request, const char *txn_id) {
    (void)ctx;
    if (g_pending_count >= MAX_PENDING_DEPOSITS) {
        return STATUS_QUEUE_FULL;
    }

    pending_deposit_t *p = &g_pending[g_pending_count++];
    p->request_id = request->request_id;
    p->client_pid = request->sender_pid;
    strncpy(p->client_id.value, request->client_id, CLIENT_ID_LENGTH);
    p->client_id.value[CLIENT_ID_LENGTH] = '\0';
    p->amount = request->amount;
    strncpy(p->txn_id, txn_id, MAX_TRANSACTION_ID);
    p->active = 1;
    p->created_at = (uint64_t)time(NULL);

    printf("\n[OPERATOR ALERT] New Pending Cash Deposit:\n");
    printf("  Transaction ID: %s\n", p->txn_id);
    printf("  Client ID:      %s\n", p->client_id.value);
    printf("  Amount:         %" PRId64 " %s\n", p->amount, CURRENCY_CODE);
    printf("  Type 'approve %s' or 'reject %s' to process.\n\n", p->txn_id, p->txn_id);
    fflush(stdout);

    return STATUS_TRANSACTION_PENDING;
}

status_t transaction_approve_deposit(server_ctx_t *ctx, const char *txn_id) {
    if (ctx == NULL || txn_id == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    pending_deposit_t *target = NULL;
    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i].active && strcmp(g_pending[i].txn_id, txn_id) == 0) {
            target = &g_pending[i];
            break;
        }
    }

    if (target == NULL) {
        return STATUS_NOT_FOUND;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    account_t *account = registry_find(ctx->bank, &target->client_id);
    status_t result = STATUS_NOT_FOUND;
    money_t new_bal = 0;

    if (account != NULL) {
        result = do_deposit(ctx, account, target->amount);
        new_bal = account->balance;
    }

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    target->active = 0;

    /* Log committed transaction to WAL */
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    strncpy(tx.transaction_id.value, target->txn_id, MAX_TRANSACTION_ID);
    snprintf(tx.request_id.value, sizeof(tx.request_id.value), "%u", target->request_id);
    tx.type = TRANSACTION_DEPOSIT;
    tx.sender_id = target->client_id;
    tx.amount = target->amount;
    tx.state = (result == STATUS_OK) ? TRANSACTION_COMMITTED : TRANSACTION_FAILED;
    tx.result = result;
    tx.completed_at = (uint64_t)time(NULL);
    persistence_log_transaction(&tx);

    record_idempotency(target->request_id, &target->client_id, MSG_DEPOSIT, result, new_bal, target->txn_id);

    printf("Deposit %s approved for %s: %" PRId64 " %s (new balance: %" PRId64 ")\n",
           target->txn_id, target->client_id.value, target->amount, CURRENCY_CODE, new_bal);
    fflush(stdout);

    return result;
}

status_t transaction_reject_deposit(server_ctx_t *ctx, const char *txn_id) {
    (void)ctx;
    if (txn_id == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i].active && strcmp(g_pending[i].txn_id, txn_id) == 0) {
            g_pending[i].active = 0;

            transaction_t tx;
            memset(&tx, 0, sizeof(tx));
            strncpy(tx.transaction_id.value, g_pending[i].txn_id, MAX_TRANSACTION_ID);
            snprintf(tx.request_id.value, sizeof(tx.request_id.value), "%u", g_pending[i].request_id);
            tx.type = TRANSACTION_DEPOSIT;
            tx.sender_id = g_pending[i].client_id;
            tx.amount = g_pending[i].amount;
            tx.state = TRANSACTION_REJECTED;
            tx.result = STATUS_TRANSACTION_REJECTED;
            tx.completed_at = (uint64_t)time(NULL);
            persistence_log_transaction(&tx);

            record_idempotency(g_pending[i].request_id, &g_pending[i].client_id, MSG_DEPOSIT,
                               STATUS_TRANSACTION_REJECTED, 0, g_pending[i].txn_id);

            printf("Deposit %s rejected for %s\n", txn_id, g_pending[i].client_id.value);
            fflush(stdout);
            return STATUS_OK;
        }
    }

    return STATUS_NOT_FOUND;
}

int transaction_get_pending(pending_deposit_t *list, int max_count) {
    int count = 0;
    for (int i = 0; i < g_pending_count && count < max_count; i++) {
        if (g_pending[i].active) {
            list[count++] = g_pending[i];
        }
    }
    return count;
}

status_t transaction_status_query(server_ctx_t *ctx, uint32_t request_id, Message *response) {
    (void)ctx;
    client_id_t cid;
    memset(&cid, 0, sizeof(cid));
    strncpy(cid.value, response->client_id, CLIENT_ID_LENGTH);

    idempotency_entry_t entry;
    if (find_idempotency(request_id, &cid, &entry)) {
        response->status_code = entry.status;
        response->amount = entry.final_balance;
        strncpy(response->txn_id, entry.txn_id, TXN_ID_LEN);
        return STATUS_OK;
    }

    response->status_code = STATUS_NOT_FOUND;
    return STATUS_NOT_FOUND;
}

status_t transaction_execute(server_ctx_t *ctx, const Message *request, Message *response) {
    if (ctx == NULL || request == NULL || response == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    client_id_t sender_id, receiver_id;
    memset(&sender_id, 0, sizeof(sender_id));
    memset(&receiver_id, 0, sizeof(receiver_id));
    strncpy(sender_id.value, request->client_id, CLIENT_ID_LENGTH);
    strncpy(receiver_id.value, request->receiver_id, CLIENT_ID_LENGTH);

    /* Idempotency check */
    idempotency_entry_t cached;
    if (find_idempotency(request->request_id, &sender_id, &cached)) {
        response->status_code = cached.status;
        response->amount = cached.final_balance;
        strncpy(response->txn_id, cached.txn_id, TXN_ID_LEN);
        return cached.status;
    }

    transaction_id_t txn_id;
    next_transaction_id(ctx, &txn_id);
    strncpy(response->txn_id, txn_id.value, TXN_ID_LEN);

    if (request->amount <= 0) {
        response->status_code = STATUS_INVALID_AMOUNT;
        record_idempotency(request->request_id, &sender_id, request->operation,
                           STATUS_INVALID_AMOUNT, 0, txn_id.value);
        return STATUS_INVALID_AMOUNT;
    }

    /* Operator approval for deposits */
    if (request->operation == MSG_DEPOSIT && !ctx->auto_approve) {
        status_t st = enqueue_pending_deposit(ctx, request, txn_id.value);
        response->status_code = st;
        return st;
    }

    /* Critical Section: Acquire semaphore lock */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        response->status_code = STATUS_SYNCHRONIZATION_ERROR;
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    status_t result = STATUS_ERROR;
    account_t *sender = registry_find(ctx->bank, &sender_id);
    money_t final_bal = 0;

    if (sender == NULL) {
        result = STATUS_ACCOUNT_NOT_FOUND;
    } else if (request->operation == MSG_DEPOSIT) {
        result = do_deposit(ctx, sender, request->amount);
        final_bal = sender->balance;
    } else if (request->operation == MSG_WITHDRAW) {
        result = do_withdrawal(ctx, sender, request->amount);
        final_bal = sender->balance;
    } else if (request->operation == MSG_TRANSFER) {
        account_t *receiver = registry_find(ctx->bank, &receiver_id);
        if (receiver == NULL) {
            result = STATUS_ACCOUNT_NOT_FOUND;
        } else {
            result = do_transfer(ctx, sender, receiver, request->amount);
            final_bal = sender->balance;
        }
    } else {
        result = STATUS_INVALID_ARGUMENT;
    }

    if (sender != NULL) {
        response->amount = final_bal;
    }

    /* Release semaphore lock */
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    response->status_code = result;

    /* Write-Ahead Log */
    transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.transaction_id = txn_id;
    snprintf(tx.request_id.value, sizeof(tx.request_id.value), "%u", request->request_id);
    if (request->operation == MSG_DEPOSIT) tx.type = TRANSACTION_DEPOSIT;
    else if (request->operation == MSG_WITHDRAW) tx.type = TRANSACTION_WITHDRAWAL;
    else if (request->operation == MSG_TRANSFER) tx.type = TRANSACTION_TRANSFER;
    tx.sender_id = sender_id;
    tx.receiver_id = receiver_id;
    tx.amount = request->amount;
    tx.state = (result == STATUS_OK) ? TRANSACTION_COMMITTED : TRANSACTION_REJECTED;
    tx.result = result;
    tx.completed_at = (uint64_t)time(NULL);
    persistence_log_transaction(&tx);

    record_idempotency(request->request_id, &sender_id, request->operation, result, final_bal, txn_id.value);
    return result;
}

status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response) {
    if (ctx == NULL || id == NULL || response == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
    memset(response, 0, sizeof(*response));

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    status_t result = STATUS_ACCOUNT_NOT_FOUND;
    account_t *account = registry_find(ctx->bank, id);
    if (account != NULL) {
        response->client_id = account->client_id;
        response->balance = account->balance;
        response->account_version = account->version;
        result = STATUS_OK;
    }

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    return result;
}
