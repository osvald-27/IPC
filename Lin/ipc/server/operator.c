#include "server.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_t g_operator_thread;
static int g_operator_running = 0;

void operator_print_status(server_ctx_t *ctx) {
    if (ctx == NULL || ctx->bank == NULL) {
        return;
    }

    bank_t copy;
    money_t actual = 0, expected = 0;

    /* Copy bank state under lock so no in-flight transfer is observed half done */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return;
    }
    copy = *ctx->bank;
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    int consistent = registry_consistent(&copy, &actual, &expected);

    printf("\n  account    balance      deposits   withdrawals  version\n");
    printf("  ---------------------------------------------------------\n");
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        const account_t *a = &copy.accounts[i];
        printf("  %-8s %10" PRId64 " %12" PRId64 " %12" PRId64 " %8" PRIu64 "\n",
               a->client_id.value, a->balance, a->total_deposits, a->total_withdrawals, a->version);
    }
    printf("  ---------------------------------------------------------\n");
    printf("  total %" PRId64 " %s, expected %" PRId64 "  ->  %s\n\n",
           actual, CURRENCY_CODE, expected, consistent ? "CONSISTENT" : "NOT CONSISTENT");
    fflush(stdout);
}

static void operator_print_pending(void) {
    pending_deposit_t list[MAX_PENDING_DEPOSITS];
    int count = transaction_get_pending(list, MAX_PENDING_DEPOSITS);

    if (count == 0) {
        printf("No pending deposits awaiting approval.\n");
        fflush(stdout);
        return;
    }

    printf("\n  === PENDING CASH DEPOSITS (%d) ===\n", count);
    for (int i = 0; i < count; i++) {
        printf("  TxnID: %-16s | Client: %-8s | Amount: %" PRId64 " %s\n",
               list[i].txn_id, list[i].client_id.value, list[i].amount, CURRENCY_CODE);
    }
    printf("  Commands: 'approve <TxnID>' or 'reject <TxnID>'\n\n");
    fflush(stdout);
}

static void *operator_thread_func(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    char line[128];

    while (!ctx->stop && fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "pending") == 0) {
            operator_print_pending();
        } else if (strncmp(line, "approve ", 8) == 0) {
            const char *txn_id = line + 8;
            while (*txn_id == ' ') txn_id++;
            transaction_approve_deposit(ctx, txn_id);
        } else if (strncmp(line, "reject ", 7) == 0) {
            const char *txn_id = line + 7;
            while (*txn_id == ' ') txn_id++;
            transaction_reject_deposit(ctx, txn_id);
        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            ctx->stop = 1;
            break;
        } else if (strcmp(line, "help") == 0) {
            printf("Operator commands:\n");
            printf("  status            - show account balances and consistency check\n");
            printf("  pending           - list cash deposits awaiting approval\n");
            printf("  approve <TxnID>   - approve a pending cash deposit\n");
            printf("  reject <TxnID>    - reject a pending cash deposit\n");
            printf("  quit              - stop server gracefully\n");
            fflush(stdout);
        } else if (line[0] != '\0') {
            printf("Unknown command '%s'. Type 'help' for options.\n", line);
            fflush(stdout);
        }
    }
    return NULL;
}

void operator_start(server_ctx_t *ctx) {
    if (g_operator_running) {
        return;
    }
    if (pthread_create(&g_operator_thread, NULL, operator_thread_func, ctx) == 0) {
        g_operator_running = 1;
    } else {
        perror("operator_start: pthread_create failed");
    }
}

void operator_stop(void) {
    if (g_operator_running) {
        g_operator_running = 0;
    }
}
