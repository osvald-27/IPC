#include "server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * The operator is the person at the server console.
 * Commands: status, quit, help.
 */

void operator_print_status(server_ctx_t *ctx) {
    bank_t copy;
    money_t actual, expected;

    /* copy under the lock, print after releasing it */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return;
    }
    copy = *ctx->bank;
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    int ok = registry_consistent(&copy, &actual, &expected);

    printf("\n  account    balance      deposits   withdrawals  version\n");
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        const account_t *a = &copy.accounts[i];
        printf("  %-8s %10" PRId64 " %12" PRId64 " %12" PRId64 " %8" PRIu64 "\n",
               a->client_id.value, a->balance, a->total_deposits, a->total_withdrawals, a->version);
    }
    printf("  total %" PRId64 " %s, expected %" PRId64 "  ->  %s\n\n",
           actual, CURRENCY_CODE, expected, ok ? "CONSISTENT" : "NOT CONSISTENT");
    fflush(stdout);
}

static DWORD WINAPI operator_thread(LPVOID param) {
    server_ctx_t *ctx = param;
    char line[64];

    while (fgets(line, sizeof line, stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "quit") == 0) {
            SetEvent(ctx->stop);
            break;
        } else if (line[0] != '\0') {
            printf("commands: status, quit\n");
            fflush(stdout);
        }
    }
    return 0;
}

void operator_start(server_ctx_t *ctx) {
    HANDLE thread = CreateThread(NULL, 0, operator_thread, ctx, 0, NULL);
    if (thread == NULL) {
        fprintf(stderr, "CreateThread for operator failed: %lu\n", GetLastError());
        return;
    }
    /* the thread ends with the process, nobody waits on it */
    CloseHandle(thread);
}
