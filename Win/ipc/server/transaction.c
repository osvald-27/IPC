#include "server.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void next_transaction_id(const server_ctx_t *ctx, transaction_id_t *id) {
    static unsigned long counter = 0;

    memset(id, 0, sizeof *id);
    snprintf(id->value, sizeof id->value, "T%02d-%lu-%lu", ctx->worker_id,
             (unsigned long)GetCurrentProcessId(), ++counter);
}

/*
 * Read, then write. With the lock held nothing can come in between.
 * With --nolock the Sleep(0) gives up the processor between the read
 * and the write, so another worker can update the same balance and
 * have its update overwritten. That is the lost update.
 */
static void set_balance(const server_ctx_t *ctx, account_t *account, money_t change) {
    money_t current = account->balance;
    if (!ctx->use_lock) {
        Sleep(0);
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

/* Debit and credit happen inside one locked section, so no other
 * transaction can see the money missing from both accounts. */
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

status_t transaction_execute(server_ctx_t *ctx, const transaction_request_t *request,
                             transaction_response_t *response) {
    memset(response, 0, sizeof *response);
    next_transaction_id(ctx, &response->transaction_id);
    response->state = TRANSACTION_STATE_REJECTED;

    /* checks that do not touch shared memory are done before taking the lock */
    if (request->amount <= 0) {
        response->status = STATUS_INVALID_AMOUNT;
        return response->status;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        response->state = TRANSACTION_STATE_FAILED;
        response->status = STATUS_SYNCHRONIZATION_ERROR;
        return response->status;
    }

    status_t result;
    account_t *sender = registry_find(ctx->bank, &request->sender_id);
    if (sender == NULL) {
        result = STATUS_ACCOUNT_NOT_FOUND;
    } else if (request->type == TRANSACTION_DEPOSIT) {
        result = do_deposit(ctx, sender, request->amount);
    } else if (request->type == TRANSACTION_WITHDRAWAL) {
        result = do_withdrawal(ctx, sender, request->amount);
    } else if (request->type == TRANSACTION_TRANSFER) {
        account_t *receiver = registry_find(ctx->bank, &request->receiver_id);
        result = (receiver == NULL) ? STATUS_ACCOUNT_NOT_FOUND
                                    : do_transfer(ctx, sender, receiver, request->amount);
    } else {
        result = STATUS_INVALID_ARGUMENT;
    }
    if (sender != NULL) {
        response->balance = sender->balance;
    }

    if (ctx->use_lock && semaphore_unlock(ctx->lock) != STATUS_OK) {
        response->state = TRANSACTION_STATE_FAILED;
        response->status = STATUS_SYNCHRONIZATION_ERROR;
        return response->status;
    }

    response->status = result;
    response->state = (result == STATUS_OK) ? TRANSACTION_STATE_COMPLETED : TRANSACTION_STATE_REJECTED;
    return result;
}

status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response) {
    memset(response, 0, sizeof *response);

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

    if (ctx->use_lock && semaphore_unlock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }
    return result;
}
