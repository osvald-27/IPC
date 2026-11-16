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

static void *operator_thread_func(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    char line[128];

    while (!ctx->stop && fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "pending") == 0) {
            operator_print_pending(ctx);
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
}/*
 * operator.c (Linux Operator Console Interface)
 *
 * Runs an interactive admin loop in the parent server process so an operator
 * can inspect live account balances, audit total-funds consistency, and
 * approve or reject queued cash deposits without stalling worker IPC.
 *
 * Why this runs on a pthread in the parent process:
 * Worker processes are blocked on msgrcv() handling client traffic, while
 * fgets(stdin) blocks waiting for keyboard input. Spawning a lightweight
 * POSIX thread inside the parent process keeps stdin decoupled from the
 * worker pool while still sharing direct pointer access to ctx->bank and
 * ctx->lock in System V shared memory.
 */

#include "server.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_t g_operator_thread;
static int       g_operator_running = 0;

/*
 * Prints a formatted table of all accounts and runs the conservation-of-funds check.
 * Notice how we take a fast local copy of bank_t under the semaphore lock and
 * release the lock *before* calling printf()—holding a global IPC semaphore
 * while writing to stdout would throttle every worker process on the system.
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

/*
 * Pulls a thread-safe snapshot of unapproved cash deposits from shared memory
 * (using ctx so transaction_get_pending can lock ctx->lock before reading
 * ctx->bank->pending) and displays their transaction IDs for review.
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
 * Background console thread entry point.
 * Reads commands line-by-line from stdin until EOF or 'quit'. Setting ctx->stop = 1
 * signals the parent main() loop to start shutting down all forked workers cleanly.
 */
static void *operator_thread_func(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    char line[128];

    while (!ctx->stop && fgets(line, sizeof(line), stdin) != NULL) {
        /* Strip trailing \r or \n so commands match cleanly regardless of terminal line endings */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "pending") == 0) {
            operator_print_pending(ctx);
        } else if (strncmp(line, "approve ", 8) == 0) {
            /* Advance past "approve " and any extra spaces to isolate the transaction ID */
            const char *txn_id = line + 8;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_approve_deposit(ctx, txn_id);
        } else if (strncmp(line, "reject ", 7) == 0) {
            /* Advance past "reject " and any extra spaces to isolate the transaction ID */
            const char *txn_id = line + 7;
            while (*txn_id == ' ') {
                txn_id++;
            }
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
/*
 * operator.c (Linux Operator Console Interface)
 *
 * Runs an interactive admin loop in the parent server process so an operator
 * can inspect live account balances, audit total-funds consistency, and
 * approve or reject queued cash deposits without stalling worker IPC.
 *
 * Why this runs on a pthread in the parent process:
 * Worker processes are blocked on msgrcv() handling client traffic, while
 * fgets(stdin) blocks waiting for keyboard input. Spawning a lightweight
 * POSIX thread inside the parent process keeps stdin decoupled from the
 * worker pool while still sharing direct pointer access to ctx->bank and
 * ctx->lock in System V shared memory.
 */

#include "server.h"

#include <inttypes.h>  /* Needed for PRId64 and PRIu64 portable printf macros */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* File-scoped thread handle and guard flag so operator_start() is idempotent
 * and cannot accidentally spawn duplicate console reader threads.
 */
static pthread_t g_operator_thread;
static int       g_operator_running = 0;

/*
 * Prints a formatted table of all accounts and runs the conservation-of-funds check.
 * Notice how we take a fast local copy of bank_t under the semaphore lock and
 * release the lock *before* calling printf()—holding a global IPC semaphore
 * while writing to stdout would throttle every worker process on the system.
 */
void operator_print_status(server_ctx_t *ctx) {
    /* Guard against NULL pointers during early startup or partial teardown */
    if (ctx == NULL || ctx->bank == NULL) {
        return;
    }

    bank_t  copy;
    money_t actual   = 0;
    money_t expected = 0;

    /* Acquire the System V semaphore before reading shared memory so we never
     * read a torn snapshot mid-transfer (where money left one account but hasn't
     * landed in the other yet). Skip locking only when --nolock is active.
     */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return;
    }

    /* Shallow struct copy of the entire shared bank_t onto the stack */
    copy = *ctx->bank;

    /* Release the lock immediately before doing any slow console I/O */
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    /* Audit total funds across the copied snapshot (actual vs. expected) */
    int consistent = registry_consistent(&copy, &actual, &expected);

    printf("\n  account    balance      deposits   withdrawals  version\n");
    printf("  ---------------------------------------------------------\n");
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        const account_t *a = &copy.accounts[i];
        /* Use PRId64 / PRIu64 instead of %ld/%lu so 64-bit integers format
         * reliably across both 32-bit and 64-bit Linux/Windows builds.
         */
        printf("  %-8s %10" PRId64 " %12" PRId64 " %12" PRId64 " %8" PRIu64 "\n",
               a->client_id.value, a->balance, a->total_deposits, a->total_withdrawals, a->version);
    }
    printf("  ---------------------------------------------------------\n");
    printf("  total %" PRId64 " %s, expected %" PRId64 "  ->  %s\n\n",
           actual, CURRENCY_CODE, expected, consistent ? "CONSISTENT" : "NOT CONSISTENT");

    /* Flush stdout explicitly so output appears right away even if stdout is
     * piped or redirected to a test harness rather than an interactive TTY.
     */
    fflush(stdout);
}

/*
 * Pulls a thread-safe snapshot of unapproved cash deposits from shared memory
 * and prints their transaction IDs so the operator knows what to approve/reject.
 */
static void operator_print_pending(server_ctx_t *ctx) {
    /* Stack buffer sized to the maximum possible queue capacity */
    pending_deposit_t list[MAX_PENDING_DEPOSITS];

    /* transaction_get_pending() acquires ctx->lock internally, copies active
     * slots out of ctx->bank->pending, and returns how many were found.
     */
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
 * Background console thread entry point.
 * Loops reading commands from stdin until EOF (Ctrl+D) or 'quit' is entered.
 */
static void *operator_thread_func(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    char line[128];

    /* Check ctx->stop before every read so a SIGINT/SIGTERM shuts this loop down
     * cleanly instead of hanging forever on stdin.
     */
    while (!ctx->stop && fgets(line, sizeof(line), stdin) != NULL) {
        /* Find the first '\r' or '\n' and overwrite it with '\0' so command
         * comparisons work identically on Unix (LF) and Windows/SSH (CRLF) input.
         */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "pending") == 0) {
            operator_print_pending(ctx);
        } else if (strncmp(line, "approve ", 8) == 0) {
            /* Pointer arithmetic: skip the 8-byte "approve " prefix, then trim
             * any accidental extra spaces before passing the TxnID string.
             */
            const char *txn_id = line + 8;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_approve_deposit(ctx, txn_id);
        } else if (strncmp(line, "reject ", 7) == 0) {
            /* Skip the 7-byte "reject " prefix and any leading whitespace */
            const char *txn_id = line + 7;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_reject_deposit(ctx, txn_id);
        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            /* Flip the volatile sig_atomic_t stop flag; the parent process's
             * main loop polls this every 100ms and will trigger SIGTERM + IPC cleanup.
             */
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
            /* Ignore blank lines (just pressing Enter), but warn on typos */
            printf("Unknown command '%s'. Type 'help' for options.\n", line);
            fflush(stdout);
        }
    }

    return NULL;
}

/*
 * operator.c (Linux Operator Console Interface)
 *
 * Runs an interactive admin loop in the parent server process so an operator
 * can inspect live account balances, audit total-funds consistency, and
 * approve or reject queued cash deposits without stalling worker IPC.
 *
 * Why this runs on a pthread in the parent process:
 * Worker processes are blocked on msgrcv() handling client traffic, while
 * fgets(stdin) blocks waiting for keyboard input. Spawning a lightweight
 * POSIX thread inside the parent process keeps stdin decoupled from the
 * worker pool while still sharing direct pointer access to ctx->bank and
 * ctx->lock in System V shared memory.
 */

#include "server.h"

#include <inttypes.h>  /* Needed for PRId64 and PRIu64 portable printf macros */
#include <pthread.h>   /* POSIX threads for background console input */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* File-scoped thread handle and guard flag so operator_start() is idempotent
 * and cannot accidentally spawn duplicate console reader threads.
 */
static pthread_t g_operator_thread;
static int       g_operator_running = 0;

/*
 * Prints a formatted table of all accounts and runs the conservation-of-funds check.
 * Notice how we take a fast local copy of bank_t under the semaphore lock and
 * release the lock *before* calling printf()—holding a global IPC semaphore
 * while writing to stdout would throttle every worker process on the system.
 */
void operator_print_status(server_ctx_t *ctx) {
    /* Guard against NULL pointers during early startup or partial teardown */
    if (ctx == NULL || ctx->bank == NULL) {
        return;
    }

    bank_t  copy;
    money_t actual   = 0;
    money_t expected = 0;

    /* Acquire the System V semaphore before reading shared memory so we never
     * read a torn snapshot mid-transfer (where money left one account but hasn't
     * landed in the other yet). Skip locking only when --nolock is active.
     */
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return;
    }

    /* Shallow struct copy of the entire shared bank_t onto the local stack */
    copy = *ctx->bank;

    /* Release the lock immediately before doing any slow console I/O */
    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    /* Audit total funds across the copied snapshot (actual vs. expected) */
    int consistent = registry_consistent(&copy, &actual, &expected);

    printf("\n  account    balance      deposits   withdrawals  version\n");
    printf("  ---------------------------------------------------------\n");
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        const account_t *a = &copy.accounts[i];
        /* Use PRId64 / PRIu64 instead of %ld/%lu so 64-bit integers format
         * reliably across both 32-bit and 64-bit Linux/Windows builds.
         */
        printf("  %-8s %10" PRId64 " %12" PRId64 " %12" PRId64 " %8" PRIu64 "\n",
               a->client_id.value, a->balance, a->total_deposits, a->total_withdrawals, a->version);
    }
    printf("  ---------------------------------------------------------\n");
    printf("  total %" PRId64 " %s, expected %" PRId64 "  ->  %s\n\n",
           actual, CURRENCY_CODE, expected, consistent ? "CONSISTENT" : "NOT CONSISTENT");

    /* Flush stdout explicitly so output appears right away even if stdout is
     * piped or redirected to a test harness rather than an interactive TTY.
     */
    fflush(stdout);
}

/*
 * Pulls a thread-safe snapshot of unapproved cash deposits from shared memory
 * and prints their transaction IDs so the operator knows what to approve/reject.
 */
static void operator_print_pending(server_ctx_t *ctx) {
    /* Stack buffer sized to the maximum possible queue capacity */
    pending_deposit_t list[MAX_PENDING_DEPOSITS];

    /* transaction_get_pending() acquires ctx->lock internally, copies active
     * slots out of ctx->bank->pending, and returns how many were found.
     */
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
 * Background console thread entry point.
 * Loops reading commands from stdin until EOF (Ctrl+D) or 'quit' is entered.
 */
static void *operator_thread_func(void *arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;
    char line[128];

    /* Check ctx->stop before every read so a SIGINT/SIGTERM shuts this loop down
     * cleanly instead of hanging forever on stdin.
     */
    while (!ctx->stop && fgets(line, sizeof(line), stdin) != NULL) {
        /* Find the first '\r' or '\n' and overwrite it with '\0' so command
         * comparisons work identically on Unix (LF) and Windows/SSH (CRLF) input.
         */
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "status") == 0) {
            operator_print_status(ctx);
        } else if (strcmp(line, "pending") == 0) {
            operator_print_pending(ctx);
        } else if (strncmp(line, "approve ", 8) == 0) {
            /* Pointer arithmetic: skip the 8-byte "approve " prefix, then trim
             * any accidental extra spaces before passing the TxnID string.
             */
            const char *txn_id = line + 8;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_approve_deposit(ctx, txn_id);
        } else if (strncmp(line, "reject ", 7) == 0) {
            /* Skip the 7-byte "reject " prefix and any leading whitespace */
            const char *txn_id = line + 7;
            while (*txn_id == ' ') {
                txn_id++;
            }
            transaction_reject_deposit(ctx, txn_id);
        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            /* Flip the volatile sig_atomic_t stop flag; the parent process's
             * main loop polls this every 100ms and will trigger SIGTERM + IPC cleanup.
             */
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
            /* Ignore blank lines (just pressing Enter), but warn on typos */
            printf("Unknown command '%s'. Type 'help' for options.\n", line);
            fflush(stdout);
        }
    }

    return NULL;
}

/*
 * Spawns the interactive operator console thread.
 * Calling pthread_detach() immediately tells the OS to automatically reclaim
 * the thread's stack and resources when it exits, so main() never has to block
 * on pthread_join() during shutdown.
 */
void operator_start(server_ctx_t *ctx) {
    /* Prevent spawning a second stdin reader if called more than once */
    if (g_operator_running) {
        return;
    }

    if (pthread_create(&g_operator_thread, NULL, operator_thread_func, ctx) == 0) {
        pthread_detach(g_operator_thread);
        g_operator_running = 1;
    } else {
        perror("operator_start: pthread_create failed");
    }
}

/*
 * Marks the operator subsystem stopped during server shutdown.
 * We intentionally avoid pthread_cancel() or pthread_join() here: if the server
 * was stopped via SIGINT (Ctrl+C), the thread is likely blocked inside fgets(stdin),
 * and cancelling a thread mid-stdio call can leave internal libc locks held while
 * main() prints the final consistency report. Process exit cleans up stdin cleanly.
 */
void operator_stop(void) {
    g_operator_running = 0;
}
