#include "server_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server - Persistence & Consistency Audit
 * Owner: @William (Win/ipc/server)
 *
 * Records completed transactions to bank_audit.log and tracks exact net
 * deposit/withdrawal delta and high-resolution QueryPerformanceCounter timing
 * so the server verifies (final_total == initial_total + net_delta) and
 * reports throughput (tx/sec) on shutdown.
 * ============================================================================ */

static FILE *g_audit_log = NULL;

status_t persistence_init(server_context_t *ctx) {
    if (!ctx) {
        return STATUS_INVALID_ARGUMENT;
    }

    g_audit_log = fopen("bank_audit.log", "a");
    if (!g_audit_log) {
        /* Non-fatal: server continues even if directory is read-only */
        fprintf(stderr, "[persistence] Warning: could not open bank_audit.log\n");
    }
    return STATUS_OK;
}

void persistence_record_tx(server_context_t *ctx,
                           const transaction_request_t *req,
                           const transaction_response_t *resp,
                           money_t net_delta_change) {
    if (!ctx || !req || !resp) {
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    EnterCriticalSection(&ctx->audit_cs);
    if (!ctx->has_tx) {
        ctx->first_tx_qpc = now;
        ctx->has_tx = 1;
    }
    ctx->last_tx_qpc = now;
    ctx->net_delta += net_delta_change;
    ctx->total_transactions++;

    if (g_audit_log) {
        const char *op_name = "UNKNOWN";
        if (req->type == TRANSACTION_DEPOSIT) op_name = "DEPOSIT";
        else if (req->type == TRANSACTION_WITHDRAWAL) op_name = "WITHDRAW";
        else if (req->type == TRANSACTION_TRANSFER) op_name = "TRANSFER";

        fprintf(g_audit_log, "%s %s src=%s dst=%s amount=%" PRId64 " bal=%" PRId64 "\n",
                resp->transaction_id.value,
                op_name,
                req->sender_id.value,
                req->receiver_id.value,
                (int64_t)req->amount,
                (int64_t)resp->balance);
        fflush(g_audit_log);
    }
    LeaveCriticalSection(&ctx->audit_cs);
}

void persistence_record_failure(server_context_t *ctx) {
    if (!ctx) {
        return;
    }
    EnterCriticalSection(&ctx->audit_cs);
    ctx->failed_transactions++;
    LeaveCriticalSection(&ctx->audit_cs);
}

void persistence_print_report(server_context_t *ctx) {
    if (!ctx || !ctx->bank) {
        return;
    }

    double elapsed = 0.0;
    if (ctx->has_tx && ctx->qpc_freq.QuadPart > 0) {
        elapsed = (double)(ctx->last_tx_qpc.QuadPart - ctx->first_tx_qpc.QuadPart)
                / (double)ctx->qpc_freq.QuadPart;
    }

    money_t actual_total = 0;
    for (uint32_t i = 0; i < ACCOUNT_COUNT; i++) {
        actual_total += ctx->bank->accounts[i].balance;
    }
    money_t expected_total = ctx->initial_total + ctx->net_delta;
    double throughput = (elapsed > 0.0)
        ? ((double)ctx->total_transactions / elapsed)
        : 0.0;

    printf("\n=================== CrossIPC Windows Server Report ===================\n");
    printf("Sync Mode            : %s\n",
           ctx->sync_enabled ? "ENABLED (Win32 Named Semaphore)"
                             : "DISABLED (--no-sync race demo)");
    printf("Accounts             : %u\n", (unsigned int)ACCOUNT_COUNT);
    printf("Completed Tx         : %llu (Rejected/Failed: %llu)\n",
           (unsigned long long)ctx->total_transactions,
           (unsigned long long)ctx->failed_transactions);
    printf("Elapsed Time         : %.6f sec\n", elapsed);
    printf("Measured Throughput  : %.2f tx/sec\n", throughput);
    printf("Initial Total (%s)  : %" PRId64 "\n", CURRENCY_CODE, (int64_t)ctx->initial_total);
    printf("Net Deposit/Withdraw : %" PRId64 "\n", (int64_t)ctx->net_delta);
    printf("Expected Final Total : %" PRId64 "\n", (int64_t)expected_total);
    printf("Actual Final Total   : %" PRId64 "\n", (int64_t)actual_total);
    printf("Consistency Check    : %s\n",
           (actual_total == expected_total) ? "PASSED (final == expected)"
                                            : "FAILED (lost update detected!)");
    for (uint32_t i = 0; i < ACCOUNT_COUNT; i++) {
        printf("  Account %-7s    : %" PRId64 " %s (v%llu)\n",
               ctx->bank->accounts[i].client_id.value,
               (int64_t)ctx->bank->accounts[i].balance,
               CURRENCY_CODE,
               (unsigned long long)ctx->bank->accounts[i].version);
    }
    printf("======================================================================\n");
}

void persistence_close(void) {
    if (g_audit_log) {
        fclose(g_audit_log);
        g_audit_log = NULL;
    }
}
