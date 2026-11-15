#include "server_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server - Transaction Engine
 * Owner: @William (Win/ipc/server)
 *
 * Executes BALANCE, DEPOSIT, WITHDRAWAL, and TRANSFER operations on bank_t
 * inside Win32 Shared Memory (CreateFileMappingA / MapViewOfFile), guarded by
 * the Win32 Named Semaphore (WaitForSingleObject / ReleaseSemaphore).
 * ============================================================================ */

static volatile LONG g_tx_seq = 0;

static account_t *find_account_unlocked(bank_t *bank, const client_id_t *id) {
    if (!bank || !id) {
        return NULL;
    }
    for (uint32_t i = 0; i < ACCOUNT_COUNT; i++) {
        if (strncmp(bank->accounts[i].client_id.value,
                    id->value,
                    CLIENT_ID_LENGTH) == 0) {
            return &bank->accounts[i];
        }
    }
    return NULL;
}

static void generate_tx_id(transaction_id_t *out_id) {
    LONG seq = InterlockedIncrement(&g_tx_seq);
    memset(out_id, 0, sizeof(*out_id));
    snprintf(out_id->value, sizeof(out_id->value),
             "TX-%08ld-%06lu",
             (long)seq,
             (unsigned long)GetTickCount());
}

static status_t lock_bank(server_context_t *ctx) {
    if (!ctx->sync_enabled) {
        return STATUS_OK;
    }
    DWORD res = WaitForSingleObject(ctx->sem_handle, INFINITE);
    if (res != WAIT_OBJECT_0 && res != WAIT_ABANDONED) {
        pipe_log_win_error("WaitForSingleObject(bank semaphore)");
        return STATUS_SYNCHRONIZATION_ERROR;
    }
    return STATUS_OK;
}

static void unlock_bank(server_context_t *ctx) {
    if (!ctx->sync_enabled) {
        return;
    }
    if (!ReleaseSemaphore(ctx->sem_handle, 1, NULL)) {
        pipe_log_win_error("ReleaseSemaphore(bank semaphore)");
    }
}

status_t tx_handle_balance(server_context_t *ctx,
                           const balance_request_t *req,
                           balance_response_t *out_resp) {
    if (!ctx || !ctx->bank || !req || !out_resp) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(out_resp, 0, sizeof(*out_resp));

    status_t lock_st = lock_bank(ctx);
    if (lock_st != STATUS_OK) {
        return lock_st;
    }

    account_t *acc = find_account_unlocked(ctx->bank, &req->client_id);
    if (!acc) {
        unlock_bank(ctx);
        return STATUS_ACCOUNT_NOT_FOUND;
    }

    out_resp->client_id = acc->client_id;
    out_resp->balance = acc->balance;
    out_resp->account_version = acc->version;

    unlock_bank(ctx);
    return STATUS_OK;
}

status_t tx_handle_operation(server_context_t *ctx,
                             const transaction_request_t *req,
                             transaction_response_t *out_resp) {
    if (!ctx || !ctx->bank || !req || !out_resp) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(out_resp, 0, sizeof(*out_resp));
    generate_tx_id(&out_resp->transaction_id);
    out_resp->state = TRANSACTION_STATE_FAILED;

    if (req->amount <= 0) {
        out_resp->status = STATUS_INVALID_AMOUNT;
        out_resp->state = TRANSACTION_STATE_REJECTED;
        persistence_record_failure(ctx);
        return STATUS_INVALID_AMOUNT;
    }

    status_t lock_st = lock_bank(ctx);
    if (lock_st != STATUS_OK) {
        out_resp->status = lock_st;
        persistence_record_failure(ctx);
        return lock_st;
    }

    status_t op_status = STATUS_OK;
    money_t net_delta_change = 0;

    switch (req->type) {
        case TRANSACTION_DEPOSIT: {
            account_t *acc = find_account_unlocked(ctx->bank, &req->sender_id);
            if (!acc) {
                op_status = STATUS_ACCOUNT_NOT_FOUND;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else if (acc->balance > INT64_MAX - req->amount) {
                op_status = STATUS_BALANCE_OVERFLOW;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else {
                money_t cur = acc->balance;
                if (!ctx->sync_enabled) {
                    /* Yield CPU in --no-sync mode to expose lost-update race */
                    Sleep(1);
                }
                acc->balance = cur + req->amount;
                acc->total_deposits += req->amount;
                acc->version++;
                out_resp->balance = acc->balance;
                out_resp->state = TRANSACTION_STATE_COMPLETED;
                net_delta_change = req->amount;
            }
            break;
        }
        case TRANSACTION_WITHDRAWAL: {
            account_t *acc = find_account_unlocked(ctx->bank, &req->sender_id);
            if (!acc) {
                op_status = STATUS_ACCOUNT_NOT_FOUND;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else if (acc->balance < req->amount) {
                op_status = STATUS_INSUFFICIENT_FUNDS;
                out_resp->balance = acc->balance;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else {
                money_t cur = acc->balance;
                if (!ctx->sync_enabled) {
                    Sleep(1);
                }
                acc->balance = cur - req->amount;
                acc->total_withdrawals += req->amount;
                acc->version++;
                out_resp->balance = acc->balance;
                out_resp->state = TRANSACTION_STATE_COMPLETED;
                net_delta_change = -req->amount;
            }
            break;
        }
        case TRANSACTION_TRANSFER: {
            if (strncmp(req->sender_id.value,
                        req->receiver_id.value,
                        CLIENT_ID_LENGTH) == 0) {
                op_status = STATUS_SELF_TRANSFER;
                out_resp->state = TRANSACTION_STATE_REJECTED;
                break;
            }
            account_t *src = find_account_unlocked(ctx->bank, &req->sender_id);
            account_t *dst = find_account_unlocked(ctx->bank, &req->receiver_id);
            if (!src || !dst) {
                op_status = STATUS_ACCOUNT_NOT_FOUND;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else if (src->balance < req->amount) {
                op_status = STATUS_INSUFFICIENT_FUNDS;
                out_resp->balance = src->balance;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else if (dst->balance > INT64_MAX - req->amount) {
                op_status = STATUS_BALANCE_OVERFLOW;
                out_resp->state = TRANSACTION_STATE_REJECTED;
            } else {
                money_t src_bal = src->balance;
                money_t dst_bal = dst->balance;
                if (!ctx->sync_enabled) {
                    Sleep(1);
                }
                src->balance = src_bal - req->amount;
                dst->balance = dst_bal + req->amount;
                src->total_withdrawals += req->amount;
                dst->total_deposits += req->amount;
                src->version++;
                dst->version++;
                out_resp->balance = src->balance;
                out_resp->state = TRANSACTION_STATE_COMPLETED;
                net_delta_change = 0;
            }
            break;
        }
        default: {
            op_status = STATUS_INVALID_ARGUMENT;
            out_resp->state = TRANSACTION_STATE_REJECTED;
            break;
        }
    }

    unlock_bank(ctx);

    out_resp->status = op_status;
    if (op_status == STATUS_OK) {
        persistence_record_tx(ctx, req, out_resp, net_delta_change);
    } else {
        persistence_record_failure(ctx);
    }

    return op_status;
}
