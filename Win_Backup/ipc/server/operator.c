#include "server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Keep the Win32 thread handle private to this file so operator_stop() can
 * clean it up properly without exposing Windows threading details to main().
 */
static HANDLE g_operator_thread  = NULL;
static int    g_operator_running = 0;

/*
 * Prints a full table of accounts and runs the total-funds consistency audit.
 * Notice how we take a fast struct copy of shared memory while holding the lock,
 * then release the lock *before* calling printf(). Doing slow console I/O inside
 * the critical section would stall every worker thread processing client requests.
 */
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

/* Pulls a snapshot of unapproved cash deposits so the admin knows which
 * transaction IDs are waiting to be approved or rejected.
 */
static void operator_print_pending(server_ctx_t *ctx) {
    pending_deposit_t list[MAX_PENDING_DEPOSITS];
    int count = transaction_get_pending(ctx, list, MAX_PENDING_DEPOSITS);

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

/*
 * Dedicated console loop running on a background Win32 thread.
 * Since fgets() blocks waiting for keyboard input, isolating it here lets the
 * admin inspect state or approve deposits interactively while pipe workers keep running.
 */
static DWORD WINAPI operator_thread_func(LPVOID arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    char line[128];

    while (!g_server_stop && fgets(line, sizeof(line), stdin) != NULL) {
        /* Shutdown may be requested while fgets() is blocked on console input. */
        if (g_server_stop) break;
        /* Strip both \r and \n since Windows consoles emit CRLF line endings */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "pending") == 0) {
            operator_print_pending(ctx);
        } else if (strncmp(line, "approve ", 8) == 0) {
            /* Skip any extra spaces between the command and the transaction ID */
            const char *txn_id = line + 8;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_approve_deposit(ctx, txn_id);
        } else if (strncmp(line, "reject ", 7) == 0) {
            const char *txn_id = line + 7;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_reject_deposit(ctx, txn_id);
        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            /* Atomically flip the shared stop flag so main and worker threads shut down cleanly */
            InterlockedExchange(&g_server_stop, 1);
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

    return 0;
}

/* Spawns the operator console thread (guarded so calling it twice won't leak handles) */
void operator_start(server_ctx_t *ctx) {
    if (g_operator_running) {
        return;
    }

    g_operator_thread = CreateThread(NULL, 0, operator_thread_func, ctx, 0, NULL);
    if (g_operator_thread != NULL) {
        g_operator_running = 1;
    } else {
        fprintf(stderr, "operator_start: CreateThread failed (error %lu)\n", GetLastError());
    }
}

/*
 * Closes the thread handle without blocking on WaitForSingleObject, because
 * fgets(stdin) stays blocked on console input until the user hits Enter or the
 * process exits. Closing the handle releases the kernel object cleanly on shutdown.
 */
void operator_stop(void) {
    if (g_operator_running) {
        if (g_operator_thread != NULL) {
            /* Interrupt a pending synchronous console read, then join the thread. */
            CancelSynchronousIo(g_operator_thread);
            WaitForSingleObject(g_operator_thread, 1000);
            CloseHandle(g_operator_thread);
            g_operator_thread = NULL;
        }
        g_operator_running = 0;
    }
}

