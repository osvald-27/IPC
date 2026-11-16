/*
 * cli.c (Interactive Command-Line Interface)
 *
 * Parses user commands, checks basic input syntax, invokes session.c operations,
 * and formats server responses into readable messages. Never modifies balances
 * directly—all financial state stays strictly on the server.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/types.h"
#include "../common/status.h"

/* Session functions provided by session.c */
const char *session_client_id(void);
uint32_t    session_last_request_id(void);
status_t    session_balance(money_t *out_balance);
status_t    session_deposit(money_t amount, money_t *out_balance, char *out_txn_id, size_t txn_len);
status_t    session_withdraw(money_t amount, money_t *out_balance, char *out_txn_id, size_t txn_len);
status_t    session_transfer(const char *receiver_id, money_t amount, money_t *out_balance, char *out_txn_id, size_t txn_len);
status_t    session_status_query(uint32_t request_id, status_t *out_txn_status, money_t *out_balance, char *out_txn_id, size_t txn_len);

/* Translates status_t return codes into clear messages for the user */
const char *cli_status_text(status_t s) {
    switch (s) {
    case STATUS_OK:                    return "Success";
    case STATUS_INVALID_ARGUMENT:      return "Invalid command or argument";
    case STATUS_NOT_FOUND:             return "Record not found";
    case STATUS_PERMISSION_DENIED:     return "Permission denied";
    case STATUS_AUTH_FAILED:           return "Authentication failed (unknown account ID)";
    case STATUS_SESSION_INVALID:       return "Invalid or expired session";
    case STATUS_ACCOUNT_NOT_FOUND:     return "Target account not found";
    case STATUS_INVALID_AMOUNT:        return "Amount must be greater than zero";
    case STATUS_INSUFFICIENT_FUNDS:    return "Insufficient funds";
    case STATUS_TRANSACTION_REJECTED:  return "Transaction rejected by operator";
    case STATUS_TRANSACTION_FAILED:    return "Transaction failed";
    case STATUS_TRANSACTION_PENDING:   return "Pending operator approval";
    case STATUS_SELF_TRANSFER:         return "Cannot transfer funds to your own account";
    case STATUS_BALANCE_OVERFLOW:      return "Balance overflow limit reached";
    case STATUS_SERVER_UNAVAILABLE:    return "Banking server is not running";
    case STATUS_INTERRUPTED:           return "Operation interrupted";
    default:                           return "Unexpected error";
    }
}

static void print_help(void) {
    printf("Available commands:\n");
    printf("  balance                     - Check current account balance\n");
    printf("  deposit <amount>            - Submit a cash deposit (in %s)\n", CURRENCY_CODE);
    printf("  withdraw <amount>           - Withdraw funds immediately (in %s)\n", CURRENCY_CODE);
    printf("  transfer <ACC_ID> <amount>  - Transfer funds to another account\n");
    printf("  status [req_id]             - Check outcome of last (or specific) request\n");
    printf("  help                        - Show this command list\n");
    printf("  logout / quit               - End session and exit\n");
}

/* Parses a positive 64-bit integer amount safely without accepting trailing junk */
static int parse_amount(const char *str, money_t *out) {
    if (str == NULL || *str == '\0' || out == NULL) {
        return 0;
    }
    char *end = NULL;
    long long val = strtoll(str, &end, 10);
    if (end == str || *end != '\0' || val <= 0) {
        return 0;
    }
    *out = (money_t)val;
    return 1;
}

int cli_run(volatile int *stop_flag) {
    char line[256];

    printf("\nAuthenticated as %s. Type 'help' for commands.\n", session_client_id());

    while (!(*stop_flag)) {
        printf("crossipc (%s)> ", session_client_id());
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        char *cmd  = strtok(line, " \t");
        char *arg1 = strtok(NULL, " \t");
        char *arg2 = strtok(NULL, " \t");

        if (cmd == NULL) {
            continue;
        }

        if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "logout") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            break;
        } else if (strcmp(cmd, "balance") == 0) {
            money_t bal = 0;
            status_t st = session_balance(&bal);
            if (st == STATUS_OK) {
                printf("Current balance: %" PRId64 " %s\n", bal, CURRENCY_CODE);
            } else {
                printf("Balance check failed: %s\n", cli_status_text(st));
            }
        } else if (strcmp(cmd, "deposit") == 0) {
            money_t amt = 0;
            if (!parse_amount(arg1, &amt)) {
                printf("Usage: deposit <positive_amount>\n");
                continue;
            }
            money_t new_bal = 0;
            char txn_id[MAX_TRANSACTION_ID + 1U] = {0};
            status_t st = session_deposit(amt, &new_bal, txn_id, sizeof(txn_id));

            if (st == STATUS_TRANSACTION_PENDING) {
                printf("Deposit submitted for operator approval.\n");
                printf("  Transaction ID: %s\n", txn_id);
                printf("  Request ID:     %u (run 'status' to check approval)\n", session_last_request_id());
            } else if (st == STATUS_OK) {
                printf("Deposit successful (Txn: %s). New balance: %" PRId64 " %s\n",
                       txn_id, new_bal, CURRENCY_CODE);
            } else {
                printf("Deposit failed: %s\n", cli_status_text(st));
            }
        } else if (strcmp(cmd, "withdraw") == 0) {
            money_t amt = 0;
            if (!parse_amount(arg1, &amt)) {
                printf("Usage: withdraw <positive_amount>\n");
                continue;
            }
            money_t new_bal = 0;
            char txn_id[MAX_TRANSACTION_ID + 1U] = {0};
            status_t st = session_withdraw(amt, &new_bal, txn_id, sizeof(txn_id));

            if (st == STATUS_OK) {
                printf("Withdrawal successful (Txn: %s).\n", txn_id);
                printf("  Amount:            %" PRId64 " %s\n", amt, CURRENCY_CODE);
                printf("  Remaining balance: %" PRId64 " %s\n", new_bal, CURRENCY_CODE);
            } else {
                printf("Withdrawal failed: %s\n", cli_status_text(st));
            }
        } else if (strcmp(cmd, "transfer") == 0) {
            money_t amt = 0;
            if (arg1 == NULL || !parse_amount(arg2, &amt)) {
                printf("Usage: transfer <RECEIVER_ID> <positive_amount>\n");
                continue;
            }
            money_t new_bal = 0;
            char txn_id[MAX_TRANSACTION_ID + 1U] = {0};
            status_t st = session_transfer(arg1, amt, &new_bal, txn_id, sizeof(txn_id));

            if (st == STATUS_OK) {
                printf("Transfer successful (Txn: %s).\n", txn_id);
                printf("  To:                %s\n", arg1);
                printf("  Amount:            %" PRId64 " %s\n", amt, CURRENCY_CODE);
                printf("  Remaining balance: %" PRId64 " %s\n", new_bal, CURRENCY_CODE);
            } else {
                printf("Transfer failed: %s\n", cli_status_text(st));
            }
        } else if (strcmp(cmd, "status") == 0) {
            uint32_t target_req = session_last_request_id();
            if (arg1 != NULL) {
                target_req = (uint32_t)strtoul(arg1, NULL, 10);
            }
            if (target_req == 0) {
                printf("No previous request to query.\n");
                continue;
            }
            status_t txn_st = STATUS_ERROR;
            money_t bal = 0;
            char txn_id[MAX_TRANSACTION_ID + 1U] = {0};
            status_t st = session_status_query(target_req, &txn_st, &bal, txn_id, sizeof(txn_id));

            if (st == STATUS_OK) {
                printf("Request #%u (Txn: %s) -> %s",
                       target_req, txn_id[0] ? txn_id : "-", cli_status_text(txn_st));
                if (txn_st == STATUS_OK) {
                    printf(" (Balance: %" PRId64 " %s)", bal, CURRENCY_CODE);
                }
                printf("\n");
            } else {
                printf("Status query for #%u: %s\n", target_req, cli_status_text(st));
            }
        } else {
            printf("Unknown command '%s'. Type 'help' for options.\n", cmd);
        }
    }

    return 0;
}
