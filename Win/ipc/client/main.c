/*
 * main.c (Windows Client Entry Point)
 * Same flow as Linux. Only the Ctrl+C setup differs:
 * SetConsoleCtrlHandler() instead of sigaction().
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../common/types.h"
#include "../common/status.h"

status_t    session_connect(const char *key_path);
void        session_disconnect(void);
int         session_is_logged_in(void);
uint32_t    session_last_request_id(void);
status_t    session_login(const char *client_id_str);
status_t    session_logout(void);
status_t    session_status_query(uint32_t request_id, status_t *out_txn_status,
                                 money_t *out_balance, char *out_txn_id,
                                 size_t txn_len);
int         cli_run(volatile sig_atomic_t *stop_flag);
const char *cli_status_text(status_t s);

static volatile sig_atomic_t g_stop = 0;

/* Runs on its own thread; only set the flag. */
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        g_stop = 1;
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    const char *account_arg = NULL;
    char id_buf[32];
    status_t st;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [ACCOUNT_ID]\n", argv[0]);
            return 0;
        } else if (account_arg == NULL) {
            account_arg = argv[i];
        } else {
            fprintf(stderr, "Unexpected argument '%s'.\n", argv[i]);
            return 2;
        }
    }

    if (!SetConsoleCtrlHandler(on_ctrl, TRUE)) {
        fprintf(stderr, "SetConsoleCtrlHandler failed (error %lu)\n",
                (unsigned long)GetLastError());
        return 1;
    }

    printf("=====================================================\n");
    printf("   CrossIPC Windows Banking Client (CEF427 Group 10)  \n");
    printf("=====================================================\n");

    st = session_connect(NULL);
    if (st != STATUS_OK) {
        fprintf(stderr, "Connection failed: %s\n", cli_status_text(st));
        return 1;
    }

    if (account_arg != NULL) {
        snprintf(id_buf, sizeof(id_buf), "%s", account_arg);
    } else {
        printf("Enter Account ID: ");
        fflush(stdout);
        if (fgets(id_buf, sizeof(id_buf), stdin) == NULL) {
            session_disconnect();
            return 1;
        }
        id_buf[strcspn(id_buf, "\r\n")] = '\0';
    }

    st = session_login(id_buf);
    if (st != STATUS_OK) {
        fprintf(stderr, "Login failed for '%s': %s\n", id_buf,
                cli_status_text(st));
        session_disconnect();
        return 1;
    }

    cli_run(&g_stop);

    /* After Ctrl+C, check what happened to the last request. */
    if (g_stop && session_is_logged_in()) {
        uint32_t last_req = session_last_request_id();
        printf("\n[CLIENT] Interrupt received. Checking last request...\n");
        if (last_req > 0) {
            status_t txn_st = STATUS_ERROR;
            money_t bal = 0;
            char txn_id[MAX_TRANSACTION_ID + 1U] = {0};

            if (session_status_query(last_req, &txn_st, &bal, txn_id,
                                     sizeof(txn_id)) == STATUS_OK) {
                printf("[CLIENT] Last request #%u (Txn: %s): %s\n",
                       last_req, txn_id[0] ? txn_id : "-",
                       cli_status_text(txn_st));
            }
        }
    }

    if (session_is_logged_in()) {
        session_logout();
    }
    session_disconnect();

    printf("Disconnected cleanly.\n");
    return 0;
}
