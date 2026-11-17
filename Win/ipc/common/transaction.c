#include "server.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
static pending_deposit_t g_pending[MAX_PENDING_DEPOSITS];
static int g_idempotency_index;
static int g_pending_count;
static volatile LONG g_transaction_sequence;

static void copy_text(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

static void make_transaction_id(const server_ctx_t *ctx, char *output, size_t capacity)
{
    LONG sequence = InterlockedIncrement(&g_transaction_sequence);

    snprintf(
        output,
        capacity,
        "T%02d-%lu-%ld",
        ctx != NULL ? ctx->worker_id : 0,
        (unsigned long)GetCurrentProcessId(),
        (long)sequence
    );
}

static int lock_bank(server_ctx_t *ctx)
{
    if (ctx == NULL || ctx->bank == NULL) {
        return 0;
    }

    if (!ctx->use_lock) {
        return 1;
    }

    return semaphore_lock(ctx->lock) == STATUS_OK;
}

static void unlock_bank(server_ctx_t *ctx)
{
    if (ctx != NULL && ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }
}

static int find_idempotency_locked(
    const client_id_t *client_id,
    uint32_t request_id,
    idempotency_entry_t *result
)
{
    int i;

    if (client_id == NULL || request_id == 0U) {
        return 0;
    }

    for (i = 0; i < IDEMPOTENCY_CACHE_SIZE; ++i) {
        idempotency_entry_t *entry = &g_idempotency_cache[i];

        if (entry->request_id == request_id &&
            memcmp(entry->client_id.value, client_id->value, sizeof(client_id->value)) == 0) {
            if (result != NULL) {
                *result = *entry;
            }
            return 1;
        }
    }

    return 0;
}

static void record_idempotency_locked(
    uint32_t request_id,
    const client_id_t *client_id,
    int32_t operation,
    status_t status,
    money_t balance,
    const char *transaction_id
)
{
    idempotency_entry_t *entry;

    if (request_id == 0U || client_id == NULL) {
        return;
    }

    entry = &g_idempotency_cache[g_idempotency_index % IDEMPOTENCY_CACHE_SIZE];
    memset(entry, 0, sizeof(*entry));

    entry->request_id = request_id;
    entry->client_id = *client_id;
    entry->operation = operation;
    entry->status = status;
    entry->final_balance = balance;
    entry->timestamp = (uint64_t)time(NULL);
    copy_text(entry->txn_id, sizeof(entry->txn_id), transaction_id);

    ++g_idempotency_index;
}

static void fill_transaction_record(
    transaction_t *transaction,
    const Message *request,
    const char *transaction_id,
    status_t result
)
{
    memset(transaction, 0, sizeof(*transaction));

    copy_text(
        transaction->transaction_id.value,
        sizeof(transaction->transaction_id.value),
        transaction_id
    );

    snprintf(
        transaction->request_id.value,
        sizeof(transaction->request_id.value),
        "%u",
        request->request_id
    );

    transaction->sender_id = (client_id_t){0};
    copy_text(
        transaction->sender_id.value,
        sizeof(transaction->sender_id.value),
        request->client_id
    );

    copy_text(
        transaction->receiver_id.value,
        sizeof(transaction->receiver_id.value),
        request->receiver_id
    );

    transaction->amount = request->amount;
    transaction->result = result;
    transaction->created_at = (uint64_t)time(NULL);
    transaction->completed_at = transaction->created_at;

    if (request->operation == MSG_DEPOSIT) {
        transaction->type = TRANSACTION_DEPOSIT;
    } else if (request->operation == MSG_WITHDRAW) {
        transaction->type = TRANSACTION_WITHDRAWAL;
    } else {
        transaction->type = TRANSACTION_TRANSFER;
    }

    if (result == STATUS_OK) {
        transaction->state = TRANSACTION_COMMITTED;
    } else {
        transaction->state = TRANSACTION_REJECTED;
    }
}

static status_t credit_account(account_t *account, money_t amount)
{
    if (account == NULL || amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    if (account->balance > INT64_MAX - amount ||
        account->total_deposits > INT64_MAX - amount) {
        return STATUS_BALANCE_OVERFLOW;
    }

    account->balance += amount;
    account->total_deposits += amount;
    ++account->version;

    return STATUS_OK;
}

static status_t debit_account(account_t *account, money_t amount)
{
    if (account == NULL || amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    if (account->balance < amount) {
        return STATUS_INSUFFICIENT_FUNDS;
    }

    if (account->total_withdrawals > INT64_MAX - amount) {
        return STATUS_BALANCE_OVERFLOW;
    }

    account->balance -= amount;
    account->total_withdrawals += amount;
    ++account->version;

    return STATUS_OK;
}

static status_t transfer_between_accounts(
    account_t *sender,
    account_t *receiver,
    money_t amount
)
{
    if (sender == NULL || receiver == NULL) {
        return STATUS_ACCOUNT_NOT_FOUND;
    }

    if (sender == receiver) {
        return STATUS_SELF_TRANSFER;
    }

    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    if (sender->balance < amount) {
        return STATUS_INSUFFICIENT_FUNDS;
    }

    if (receiver->balance > INT64_MAX - amount) {
        return STATUS_BALANCE_OVERFLOW;
    }

    sender->balance -= amount;
    receiver->balance += amount;

    ++sender->version;
    ++receiver->version;

    return STATUS_OK;
}

static status_t enqueue_pending_deposit(
    server_ctx_t *ctx,
    const Message *request,
    const char *transaction_id
)
{
    pending_deposit_t *pending;

    if (!lock_bank(ctx)) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    if (g_pending_count >= MAX_PENDING_DEPOSITS) {
        unlock_bank(ctx);
        return STATUS_QUEUE_FULL;
    }

    pending = &g_pending[g_pending_count++];
    memset(pending, 0, sizeof(*pending));

    pending->request_id = request->request_id;
    pending->client_pid = request->sender_pid;
    pending->amount = request->amount;
    pending->active = 1;
    pending->created_at = (uint64_t)time(NULL);

    copy_text(
        pending->client_id.value,
        sizeof(pending->client_id.value),
        request->client_id
    );

    copy_text(
        pending->txn_id,
        sizeof(pending->txn_id),
        transaction_id
    );

    unlock_bank(ctx);

    printf(
        "\n[PENDING DEPOSIT] transaction=%s client=%s amount=%" PRId64 " %s\n",
        pending->txn_id,
        pending->client_id.value,
        pending->amount,
        CURRENCY_CODE
    );
    fflush(stdout);

    return STATUS_TRANSACTION_PENDING;
}

status_t transaction_execute(
    server_ctx_t *ctx,
    const Message *request,
    Message *response
)
{
    client_id_t sender_id;
    client_id_t receiver_id;
    account_t *sender;
    account_t *receiver = NULL;
    idempotency_entry_t cached;
    transaction_t transaction;
    char transaction_id[MAX_TRANSACTION_ID + 1U];
    money_t final_balance = 0;
    status_t result;

    if (ctx == NULL || request == NULL || response == NULL || ctx->bank == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (request->request_id == 0U) {
        response->status_code = STATUS_INVALID_ARGUMENT;
        return STATUS_INVALID_ARGUMENT;
    }

    if (request->amount <= 0) {
        response->status_code = STATUS_INVALID_AMOUNT;
        return STATUS_INVALID_AMOUNT;
    }

    memset(&sender_id, 0, sizeof(sender_id));
    memset(&receiver_id, 0, sizeof(receiver_id));

    copy_text(sender_id.value, sizeof(sender_id.value), request->client_id);
    copy_text(receiver_id.value, sizeof(receiver_id.value), request->receiver_id);

    if (!lock_bank(ctx)) {
        response->status_code = STATUS_SYNCHRONIZATION_ERROR;
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    if (find_idempotency_locked(&sender_id, request->request_id, &cached)) {
        response->status_code = cached.status;
        response->amount = cached.final_balance;
        copy_text(response->txn_id, sizeof(response->txn_id), cached.txn_id);
        unlock_bank(ctx);
        return cached.status;
    }

    unlock_bank(ctx);

    make_transaction_id(ctx, transaction_id, sizeof(transaction_id));
    copy_text(response->txn_id, sizeof(response->txn_id), transaction_id);

    if (request->operation == MSG_DEPOSIT && !ctx->auto_approve) {
        result = enqueue_pending_deposit(ctx, request, transaction_id);
        response->status_code = result;
        return result;
    }

    if (!lock_bank(ctx)) {
        response->status_code = STATUS_SYNCHRONIZATION_ERROR;
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    if (find_idempotency_locked(&sender_id, request->request_id, &cached)) {
        response->status_code = cached.status;
        response->amount = cached.final_balance;
        copy_text(response->txn_id, sizeof(response->txn_id), cached.txn_id);
        unlock_bank(ctx);
        return cached.status;
    }

    sender = registry_find(ctx->bank, &sender_id);

    if (sender == NULL) {
        result = STATUS_ACCOUNT_NOT_FOUND;
    } else if (request->operation == MSG_DEPOSIT) {
        result = credit_account(sender, request->amount);
        final_balance = sender->balance;
    } else if (request->operation == MSG_WITHDRAW) {
        result = debit_account(sender, request->amount);
        final_balance = sender->balance;
    } else if (request->operation == MSG_TRANSFER) {
        receiver = registry_find(ctx->bank, &receiver_id);
        result = transfer_between_accounts(sender, receiver, request->amount);
        final_balance = sender->balance;
    } else {
        result = STATUS_INVALID_ARGUMENT;
    }

    record_idempotency_locked(
        request->request_id,
        &sender_id,
        request->operation,
        result,
        final_balance,
        transaction_id
    );

    response->amount = final_balance;
    response->status_code = result;

    unlock_bank(ctx);

    fill_transaction_record(&transaction, request, transaction_id, result);

    if (persistence_log_transaction(&transaction) != STATUS_OK && result == STATUS_OK) {
        response->status_code = STATUS_STORAGE_ERROR;
        return STATUS_STORAGE_ERROR;
    }

    persistence_save(ctx->bank);

    return result;
}

status_t transaction_balance(
    server_ctx_t *ctx,
    const client_id_t *id,
    balance_response_t *response
)
{
    account_t *account;

    if (ctx == NULL || id == NULL || response == NULL || ctx->bank == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (!lock_bank(ctx)) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    account = registry_find(ctx->bank, id);

    if (account == NULL) {
        unlock_bank(ctx);
        return STATUS_ACCOUNT_NOT_FOUND;
    }

    memset(response, 0, sizeof(*response));
    response->client_id = account->client_id;
    response->balance = account->balance;
    response->account_version = account->version;

    unlock_bank(ctx);
    return STATUS_OK;
}

status_t transaction_status_query(
    server_ctx_t *ctx,
    uint32_t request_id,
    Message *response
)
{
    client_id_t client_id;
    idempotency_entry_t cached;

    if (ctx == NULL || response == NULL || request_id == 0U) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(&client_id, 0, sizeof(client_id));
    copy_text(client_id.value, sizeof(client_id.value), response->client_id);

    if (!lock_bank(ctx)) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    if (!find_idempotency_locked(&client_id, request_id, &cached)) {
        unlock_bank(ctx);
        response->status_code = STATUS_NOT_FOUND;
        return STATUS_NOT_FOUND;
    }

    response->status_code = cached.status;
    response->amount = cached.final_balance;
    copy_text(response->txn_id, sizeof(response->txn_id), cached.txn_id);

    unlock_bank(ctx);
    return STATUS_OK;
}

status_t transaction_approve_deposit(server_ctx_t *ctx, const char *txn_id)
{
    int i;
    pending_deposit_t saved;
    account_t *account;
    status_t result;

    if (ctx == NULL || txn_id == NULL || txn_id[0] == '\0') {
        return STATUS_INVALID_ARGUMENT;
    }

    if (!lock_bank(ctx)) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    for (i = 0; i < g_pending_count; ++i) {
        if (g_pending[i].active && strcmp(g_pending[i].txn_id, txn_id) == 0) {
            break;
        }
    }

    if (i == g_pending_count) {
        unlock_bank(ctx);
        return STATUS_NOT_FOUND;
    }

    saved = g_pending[i];
    account = registry_find(ctx->bank, &saved.client_id);

    result = account == NULL
        ? STATUS_ACCOUNT_NOT_FOUND
        : credit_account(account, saved.amount);

    g_pending[i].active = 0;

    record_idempotency_locked(
        saved.request_id,
        &saved.client_id,
        MSG_DEPOSIT,
        result,
        account != NULL ? account->balance : 0,
        saved.txn_id
    );

    unlock_bank(ctx);

    {
        transaction_t transaction;
        Message request;

        memset(&request, 0, sizeof(request));
        request.request_id = saved.request_id;
        request.operation = MSG_DEPOSIT;
        request.amount = saved.amount;
        copy_text(request.client_id, sizeof(request.client_id), saved.client_id.value);

        fill_transaction_record(&transaction, &request, saved.txn_id, result);
        persistence_log_transaction(&transaction);
        persistence_save(ctx->bank);
    }

    return result;
}

status_t transaction_reject_deposit(server_ctx_t *ctx, const char *txn_id)
{
    int i;
    pending_deposit_t saved;

    if (ctx == NULL || txn_id == NULL || txn_id[0] == '\0') {
        return STATUS_INVALID_ARGUMENT;
    }

    if (!lock_bank(ctx)) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    for (i = 0; i < g_pending_count; ++i) {
        if (g_pending[i].active && strcmp(g_pending[i].txn_id, txn_id) == 0) {
            break;
        }
    }

    if (i == g_pending_count) {
        unlock_bank(ctx);
        return STATUS_NOT_FOUND;
    }

    saved = g_pending[i];
    g_pending[i].active = 0;

    record_idempotency_locked(
        saved.request_id,
        &saved.client_id,
        MSG_DEPOSIT,
        STATUS_TRANSACTION_REJECTED,
        0,
        saved.txn_id
    );

    unlock_bank(ctx);

    {
        transaction_t transaction;
        Message request;

        memset(&request, 0, sizeof(request));
        request.request_id = saved.request_id;
        request.operation = MSG_DEPOSIT;
        request.amount = saved.amount;
        copy_text(request.client_id, sizeof(request.client_id), saved.client_id.value);

        fill_transaction_record(
            &transaction,
            &request,
            saved.txn_id,
            STATUS_TRANSACTION_REJECTED
        );

        transaction.state = TRANSACTION_REJECTED;
        persistence_log_transaction(&transaction);
    }

    return STATUS_OK;
}

int transaction_get_pending(pending_deposit_t *list, int max_count)
{
    int i;
    int count = 0;

    if (list == NULL || max_count <= 0) {
        return 0;
    }

    for (i = 0; i < g_pending_count && count < max_count; ++i) {
        if (g_pending[i].active) {
            list[count++] = g_pending[i];
        }
    }

    return count;
}
