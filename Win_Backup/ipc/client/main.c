#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common/types.h"
#include "../common/status.h"
status_t session_connect(const char *key_path);
void session_disconnect(void);
int session_is_logged_in(void);
uint32_t session_last_request_id(void);
status_t session_login(const char *client_id_str);
status_t session_logout(void);
status_t session_status_query(uint32_t request_id, status_t *out_txn_status, money_t *out_balance, char *out_txn_id, size_t txn_len);
int cli_run(volatile int *stop_flag);
const char *cli_status_text(status_t s);
static volatile int g_stop = 0;
static BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = 1; return TRUE;
    }
    return FALSE;
}
int main(int argc, char **argv) {
    const char *account = NULL;
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: %s [ACCOUNT_ID]\nExample: %s ACC0001\n", argv[0], argv[0]); return 0;
        }
        if (account) { fprintf(stderr, "Unexpected argument: %s\n", argv[i]); return 2; }
        account = argv[i];
    }
    SetConsoleCtrlHandler(console_handler, TRUE);
    puts("=====================================================\n   CrossIPC Windows Banking Client (MinGW-w64)\n=====================================================");
    status_t st = session_connect(NULL);
    if (st != STATUS_OK) { fprintf(stderr, "Connection failed: %s\n", cli_status_text(st)); return 1; }
    char id[32] = {0};
    if (account) snprintf(id, sizeof(id), "%s", account);
    else {
        printf("Enter Account ID (ACC0001, ACC0002, ACC0003): "); fflush(stdout);
        if (!fgets(id, sizeof(id), stdin)) { session_disconnect(); return 1; }
        id[strcspn(id, "\r\n")] = '\0';
    }
    st = session_login(id);
    if (st != STATUS_OK) { fprintf(stderr, "Login failed: %s\n", cli_status_text(st)); session_disconnect(); return 1; }
    cli_run(&g_stop);
    if (g_stop && session_is_logged_in()) {
        uint32_t last = session_last_request_id();
        if (last) {
            status_t txn_st = STATUS_ERROR; money_t balance = 0; char txn[MAX_TRANSACTION_ID+1] = {0};
            if (session_status_query(last, &txn_st, &balance, txn, sizeof(txn)) == STATUS_OK)
                printf("\nLast request #%u (%s): %s\n", last, txn, cli_status_text(txn_st));
        }
    }
    if (session_is_logged_in()) session_logout();
    session_disconnect(); puts("Disconnected cleanly."); return 0;
}
