#include "server_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server - Operator & Status Module
 * Owner: @William (Win/ipc/server)
 *
 * Handles REQUEST_SERVER_STATUS queries and console startup diagnostics.
 * ============================================================================ */

void operator_print_banner(const server_context_t *ctx) {
    if (!ctx) {
        return;
    }

    printf("[CrossIPC Windows Server] PID=%lu | Pipe=%s\n",
           (unsigned long)GetCurrentProcessId(), PIPE_NAME);
    printf("[CrossIPC Windows Server] SHM=%s | SEM=%s | Sync=%s\n",
           SHM_NAME, SEM_NAME, ctx->sync_enabled ? "ON" : "OFF (--no-sync)");
    printf("[CrossIPC Windows Server] Accounts initialized (%u x %" PRId64 " %s):",
           (unsigned int)ACCOUNT_COUNT, (int64_t)INITIAL_BALANCE, CURRENCY_CODE);
    if (ctx->bank) {
        for (uint32_t i = 0; i < ACCOUNT_COUNT; i++) {
            printf(" %s", ctx->bank->accounts[i].client_id.value);
        }
    }
    printf("\n[CrossIPC Windows Server] Press Ctrl+C to shut down cleanly.\n");
    fflush(stdout);
}

status_t operator_handle_status(server_context_t *ctx,
                                response_t *out_resp,
                                char *out_summary,
                                uint32_t max_len) {
    if (!ctx || !out_resp || !out_summary || max_len == 0) {
        return STATUS_INVALID_ARGUMENT;
    }

    EnterCriticalSection(&ctx->audit_cs);
    uint64_t ok_tx = ctx->total_transactions;
    uint64_t fail_tx = ctx->failed_transactions;
    LeaveCriticalSection(&ctx->audit_cs);

    uint32_t active_sessions = registry_active_count();

    int n = snprintf(out_summary, max_len,
                     "sessions=%u completed_tx=%llu failed_tx=%llu sync=%s",
                     (unsigned int)active_sessions,
                     (unsigned long long)ok_tx,
                     (unsigned long long)fail_tx,
                     ctx->sync_enabled ? "on" : "off");
    if (n < 0) {
        n = 0;
    }
    uint32_t used = ((uint32_t)n >= max_len) ? (max_len - 1U) : (uint32_t)n;

    out_resp->status = STATUS_OK;
    out_resp->payload_size = used + 1U;
    return STATUS_OK;
}
