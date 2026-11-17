#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../server/server.h"

status_t session_login(const char *client_id);
status_t session_logout(void);
status_t session_balance(money_t *balance);
status_t session_deposit(money_t amount, money_t *balance, char *transaction_id, size_t capacity);
status_t session_withdraw(money_t amount, money_t *balance, char *transaction_id, size_t capacity);
status_t session_transfer(const char *receiver_id, money_t amount, money_t *balance, char *transaction_id, size_t capacity);
status_t session_status_query(uint32_t request_id, status_t *transaction_status, money_t *balance, char *transaction_id, size_t capacity);
int session_is_logged_in(void);
const char *session_client_id(void);
uint32_t session_last_request_id(void);
const char *session_last_txn_id(void);

static const char *cli_status_text(status_t status)
{
    switch (status) {
        case STATUS_OK: return "SUCCESS";
        case STATUS_ERROR: return "ERROR";
        case STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
        case STATUS_NOT_FOUND: return "NOT_FOUND";
        case STATUS_ALREADY_EXISTS: return "ALREADY_EXISTS";
        case STATUS_PERMISSION_DENIED: return "PERMISSION_DENIED";
        case STATUS_AUTH_FAILED: return "AUTH_FAILED";
        case STATUS_SESSION_INVALID: return "SESSION_INVALID";
        case STATUS_SESSION_EXPIRED: return "SESSION_EXPIRED";
        case STATUS_ACCOUNT_NOT_FOUND: return "ACCOUNT_NOT_FOUND";
        case STATUS_ACCOUNT_EXISTS: return "ACCOUNT_EXISTS";
        case STATUS_ACCOUNT_INACTIVE: return "ACCOUNT_INACTIVE";
        case STATUS_INVALID_AMOUNT: return "INVALID_AMOUNT";
        case STATUS_INSUFFICIENT_FUNDS: return "INSUFFICIENT_FUNDS";
        case STATUS_TRANSACTION_REJECTED: return "TRANSACTION_REJECTED";
        case STATUS_TRANSACTION_FAILED: return "TRANSACTION_FAILED";
        case STATUS_TRANSACTION_PENDING: return "PENDING_APPROVAL";
        case STATUS_DUPLICATE_REQUEST: return "DUPLICATE_REQUEST";
        case STATUS_SELF_TRANSFER: return "SELF_TRANSFER";
        case STATUS_BALANCE_OVERFLOW: return "BALANCE_OVERFLOW";
        case STATUS_QUEUE_ERROR: return "IPC_ERROR";
        case STATUS_QUEUE_FULL: return "QUEUE_FULL";
        case STATUS_SHARED_MEMORY_ERROR: return "SHARED_MEMORY_ERROR";
        case STATUS_SYNCHRONIZATION_ERROR: return "SYNCHRONIZATION_ERROR";
        case STATUS_TIMEOUT: return "TIMEOUT";
        case STATUS_STORAGE_ERROR: return "STORAGE_ERROR";
        case STATUS_RECOVERY_ERROR: return "RECOVERY_ERROR";
        case STATUS_SERVER_UNAVAILABLE: return "SERVER_UNAVAILABLE";
        case STATUS_SHUTTING_DOWN: return "SERVER_SHUTTING_DOWN";
        case STATUS_INTERRUPTED: return "INTERRUPTED";
        case STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
        default: return "UNKNOWN_STATUS";
    }
}

static int parse_amount(const char *text, money_t *amount)
{
    char *end = NULL;
    long long value;

    if (text == NULL || amount == NULL || text[0] == '\0') {
        return 0;
    }

    value = strtoll(text, &end, 10);

    if (end == text || *end != '\0' || value <= 0) {
        return 0;
    }

    *amount = (money_t)value;
    return 1;
}

static void print_result(status_t status)
{
    printf("%s", cli_status_text(status));

    if (status == STATUS_TRANSACTION_PENDING) {
        printf(". Transaction is waiting for operator approval.");
    }

    printf("\n");
}

static void print_balance(void)
{
    money_t balance = 0;
    status_t status = session_balance(&balance);

    if (status == STATUS_OK) {
        printf("Available balance: %" PRId64 " %s\n", balance, CURRENCY_CODE);
    } else {
        print_result(status);
    }
}

static void process_deposit(const char *amount_text)
{
    money_t amount;
    money_t balance = 0;
    char transaction_id[TXN_ID_LEN] = {0};
    status_t status;

    if (!parse_amount(amount_text, &amount)) {
        printf("Invalid amount.\n");
        return;
    }

    status = session_deposit(
        amount,
        &balance,
        transaction_id,
        sizeof(transaction_id)
    );

    print_result(status);

    if (transaction_id[0] != '\0') {
        printf("Transaction ID: %s\n", transaction_id);
    }

    if (status == STATUS_OK) {
        printf("New balance: %" PRId64 " %s\n", balance, CURRENCY_CODE);
    }
}

static void process_withdrawal(const char *amount_text)
{
    money_t amount;
    money_t balance = 0;
    char transaction_id[TXN_ID_LEN] = {0};
    status_t status;

    if (!parse_amount(amount_text, &amount)) {
        printf("Invalid amount.\n");
        return;
    }

    status = session_withdraw(
        amount,
        &balance,
        transaction_id,
        sizeof(transaction_id)
    );

    print_result(status);

    if (transaction_id[0] != '\0') {
        printf("Transaction ID: %s\n", transaction_id);
    }

    if (status == STATUS_OK) {
        printf("New balance: %" PRId64 " %s\n", balance, CURRENCY_CODE);
    }
}

static void process_transfer(const char *receiver_id, const char *amount_text)
{
    money_t amount;
    money_t balance = 0;
    char transaction_id[TXN_ID_LEN] = {0};
    status_t status;

    if (receiver_id == NULL || strlen(receiver_id) != CLIENT_ID_LENGTH) {
        printf("Invalid receiver account ID.\n");
        return;
    }

    if (!parse_amount(amount_text, &amount)) {
        printf("Invalid amount.\n");
        return;
    }

    status = session_transfer(
        receiver_id,
        amount,
        &balance,
        transaction_id,
        sizeof(transaction_id)
    );

    print_result(status);

    if (transaction_id[0] != '\0') {
        printf("Transaction ID: %s\n", transaction_id);
    }

    if (status == STATUS_OK) {
        printf("New balance: %" PRId64 " %s\n", balance, CURRENCY_CODE);
    }
}

static void process_status(const char *request_text)
{
    char *end = NULL;
    unsigned long request_id;
    status_t transaction_status = STATUS_ERROR;
    money_t balance = 0;
    char transaction_id[TXN_ID_LEN] = {0};
    status_t status;

    if (request_text == NULL || request_text[0] == '\0') {
        request_id = session_last_request_id();
    } else {
        request_id = strtoul(request_text, &end, 10);

        if (end == request_text || *end != '\0' || request_id == 0UL || request_id > UINT32_MAX) {
            printf("Invalid request ID.\n");
            return;
        }
    }

    if (request_id == 0UL) {
        printf("No previous request is available.\n");
        return;
    }

    status = session_status_query(
        (uint32_t)request_id,
        &transaction_status,
        &balance,
        transaction_id,
        sizeof(transaction_id)
    );

    if (status != STATUS_OK) {
        print_result(status);
        return;
    }

    printf("Request ID: %lu\n", request_id);
    printf("Transaction status: %s\n", cli_status_text(transaction_status));
    printf("Balance: %" PRId64 " %s\n", balance, CURRENCY_CODE);

    if (transaction_id[0] != '\0') {
        printf("Transaction ID: %s\n", transaction_id);
    }
}

static void print_help(void)
{
    printf("Commands:\n");
    printf("  balance\n");
    printf("  deposit AMOUNT\n");
    printf("  withdraw AMOUNT\n");
    printf("  transfer ACCOUNT_ID AMOUNT\n");
    printf("  status [REQUEST_ID]\n");
    printf("  logout\n");
    printf("  help\n");
    printf("  exit\n");
}

void cli_run(void)
{
    char line[256];

    printf("\nCrossIPC Client\n");
    printf("Authenticated account: %s\n", session_client_id());
    printf("Enter help to list commands.\n\n");

    while (session_is_logged_in()) {
        char *command;
        char *first;
        char *second;

        printf("crossipc> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';

        command = strtok(line, " \t");

        if (command == NULL) {
            continue;
        }

        first = strtok(NULL, " \t");
        second = strtok(NULL, " \t");

        if (strcmp(command, "balance") == 0) {
            if (first != NULL) {
                printf("Usage: balance\n");
            } else {
                print_balance();
            }
        } else if (strcmp(command, "deposit") == 0) {
            if (first == NULL || second != NULL) {
                printf("Usage: deposit AMOUNT\n");
            } else {
                process_deposit(first);
            }
        } else if (strcmp(command, "withdraw") == 0) {
            if (first == NULL || second != NULL) {
                printf("Usage: withdraw AMOUNT\n");
            } else {
                process_withdrawal(first);
            }
        } else if (strcmp(command, "transfer") == 0) {
            if (first == NULL || second == NULL || strtok(NULL, " \t") != NULL) {
                printf("Usage: transfer ACCOUNT_ID AMOUNT\n");
            } else {
                process_transfer(first, second);
            }
        } else if (strcmp(command, "status") == 0) {
            if (second != NULL) {
                printf("Usage: status [REQUEST_ID]\n");
            } else {
                process_status(first);
            }
        } else if (strcmp(command, "logout") == 0) {
            status_t status = session_logout();
            print_result(status);
            break;
        } else if (strcmp(command, "help") == 0) {
            print_help();
        } else if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0) {
            break;
        } else {
            printf("Unknown command. Enter help.\n");
        }
    }
}
