/*
 * main.c (Linux Client Entry Point)
 *
 * Parses command-line arguments, installs async-signal-safe shutdown handlers,
 * connects to the server's System V message queue, authenticates an account ID
 * (e.g. ACC0001), runs the interactive CLI loop, and cleans up the session on
 * exit or Ctrl+C.
 *
 * Notice how main.c never touches msgget(), msgsnd(), or msgrcv() directly—it
 * delegates everything through session_*() and cli_run(). That means porting
 * this entry point to Windows only requires swapping the POSIX signal setup for
 * SetConsoleCtrlHandler() while keeping the entire flow identical.
 */

/* Expose POSIX.1-2008 APIs like sigaction() cleanly under strict -std=c11 builds */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>  /* sigaction, sigemptyset, SIGINT, SIGTERM, sig_atomic_t */
#include <stdint.h>  /* uint32_t */
#include <stdio.h>   /* printf, fprintf, fgets, fflush */
#include <string.h>  /* strcmp, strncpy, strcspn, memset */

#include "../common/types.h"
#include "../common/status.h"

/*
 * Forward declarations for the session and CLI modules.
 * Keeping these clean C interfaces decouples the client lifecycle from the
 * underlying IPC transport (System V message queue on Linux vs. Named Pipe on Windows).
 */
status_t    session_connect(const char *key_path);
void        session_disconnect(void);
int         session_is_logged_in(void);
uint32_t    session_last_request_id(void);
status_t    session_login(const char *client_id_str);
status_t    session_logout(void);
status_t    session_status_query(uint32_t request_id, status_t *out_txn_status, money_t *out_balance, char *out_txn_id, size_t txn_len);
int         cli_run(volatile sig_atomic_t *stop_flag);
const char *cli_status_text(status_t s);

/*
 * Async-signal-safe flag shared between the SIGINT/SIGTERM handler and cli_run().
 * Must be 'volatile sig_atomic_t' so the compiler never caches it in a CPU register
 * across loop iterations, and reads/writes stay atomic when a signal interrupts
 * normal execution mid-instruction.
 */
static volatile sig_atomic_t g_stop = 0;

/*
 * Keep the signal handler completely minimal—calling printf(), malloc(), or IPC
 * functions inside an asynchronous signal handler can deadlock if the signal
 * interrupted libc while holding an internal lock. Just flip g_stop and let
 * main() handle the status check and logout safely after the syscall unwinds.
 */
static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

int main(int argc, char **argv) {
    const char *account_arg = NULL;

    /* Parse optional startup account ID (e.g. ./client_app ACC0001) or --help */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [ACCOUNT_ID]\n", argv[0]);
            printf("  Example: %s ACC0001\n", argv[0]);
            return 0;
        } else if (account_arg == NULL) {
            account_arg = argv[i];
        } else {
            /* Return exit code 2 for CLI usage errors (standard Unix convention) */
            fprintf(stderr, "Unexpected argument '%s'. Use --help for usage.\n", argv[i]);
            return 2;
        }
    }

    /*
     * Install SIGINT (Ctrl+C) and SIGTERM handlers via sigaction().
     * We deliberately zero the struct and leave sa_flags = 0 (without SA_RESTART)
     * so that if the user presses Ctrl+C while the client is blocked inside
     * fgets(stdin) or msgrcv(), the kernel immediately aborts the syscall with
     * EINTR instead of automatically restarting it and hanging forever.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("=====================================================\n");
    printf("   CrossIPC Linux Banking Client (CEF427 Group 10)  \n");
    printf("=====================================================\n");

    /* Attach to the server's existing IPC queue (passing NULL uses default utilities/ipc.key) */
    status_t st = session_connect(NULL);
    if (st != STATUS_OK) {
        fprintf(stderr, "Connection failed: %s\n", cli_status_text(st));
        return 1;
    }

    /*
     * Accept the account ID from argv[1] if supplied on the command line, or
     * prompt interactively. Leaving 1 byte for '\0' after strncpy() prevents
     * buffer overruns if someone pastes an overly long string.
     */
    char id_buf[32];
    if (account_arg != NULL) {
        strncpy(id_buf, account_arg, sizeof(id_buf) - 1);
        id_buf[sizeof(id_buf) - 1] = '\0';
    } else {
        printf("Enter Account ID (e.g. ACC0001, ACC0002, ACC0003): ");
        fflush(stdout); /* Flush prompt immediately before blocking on stdin */
        if (fgets(id_buf, sizeof(id_buf), stdin) == NULL) {
            /* Handles EOF (Ctrl+D) or Ctrl+C cleanly without leaking queue state */
            session_disconnect();
            return 1;
        }
        /* Strip trailing \r or \n so the account ID string matches cleanly */
        id_buf[strcspn(id_buf, "\r\n")] = '\0';
    }

    /* Authenticate with the server and acquire an active session token */
    st = session_login(id_buf);
    if (st != STATUS_OK) {
        fprintf(stderr, "Login failed for '%s': %s\n", id_buf, cli_status_text(st));
        session_disconnect();
        return 1;
    }

    /* Enter the interactive command loop until the user types 'quit'/'logout' or g_stop flips */
    cli_run(&g_stop);

    /*
     * Client interrupt recovery:
     * If Ctrl+C interrupted an active session while a transaction was in flight,
     * the server may have already committed it before we broke out of msgrcv().
     * Never assume an interrupted request rolled back—query the server for the
     * last request_id's outcome so the user knows the true state before logging out.
     */
    if (g_stop && session_is_logged_in()) {
        printf("\n[CLIENT] Interrupt received. Checking last request status...\n");
        uint32_t last_req = session_last_request_id();
        if (last_req > 0) {
            status_t txn_st = STATUS_ERROR;
            money_t  bal    = 0;
            char     txn_id[MAX_TRANSACTION_ID + 1U] = {0};

            if (session_status_query(last_req, &txn_st, &bal, txn_id, sizeof(txn_id)) == STATUS_OK) {
                printf("[CLIENT] Last request #%u (Txn: %s): %s\n",
                       last_req, txn_id[0] ? txn_id : "-", cli_status_text(txn_st));
            }
        }
    }

    /* Notify the server to invalidate our session token and release local IPC state */
    if (session_is_logged_in()) {
        session_logout();
    }
    session_disconnect();

    printf("Disconnected cleanly.\n");
    return 0;
}
