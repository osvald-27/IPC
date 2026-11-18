#include "client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_result(const char *what, status_t st, const transaction_response_t *r) {
    if (st == STATUS_OK) {
        printf("%s done, transaction %s, balance %" PRId64 " %s\n", what, r->transaction_id.value,
               r->balance, CURRENCY_CODE);
    } else {
        printf("%s refused: %s\n", what, status_string(st));
    }
}

/* Interactive client: one person at the keyboard. */
int client_interactive(const char *client_id, HANDLE stop) {
    client_session_t s;
    char line[128];

    status_t st = session_open(&s, client_id, stop);
    if (st != STATUS_OK) {
        fprintf(stderr, "cannot log in as %s: %s\n", client_id, status_string(st));
        return 1;
    }
    printf("logged in as %s\n", client_id);
    printf("commands: balance | deposit N | withdraw N | transfer ACCxxxx N | check | quit\n");

    for (;;) {
        char command[16] = "";
        char arg1[32] = "";
        char arg2[32] = "";
        transaction_response_t tr;

        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof line, stdin) == NULL) {
            break;                                  /* end of input or Ctrl+C */
        }
        int words = sscanf(line, "%15s %31s %31s", command, arg1, arg2);
        if (words < 1) {
            continue;
        }

        if (strcmp(command, "quit") == 0) {
            break;
        } else if (strcmp(command, "balance") == 0) {
            balance_response_t b;
            st = session_balance(&s, &b);
            if (st == STATUS_OK) {
                printf("balance %" PRId64 " %s\n", b.balance, CURRENCY_CODE);
            } else {
                printf("refused: %s\n", status_string(st));
            }
        } else if (strcmp(command, "deposit") == 0 && words == 2) {
            st = session_transaction(&s, TRANSACTION_DEPOSIT, NULL, strtoll(arg1, NULL, 10), &tr);
            print_result("deposit", st, &tr);
        } else if (strcmp(command, "withdraw") == 0 && words == 2) {
            st = session_transaction(&s, TRANSACTION_WITHDRAWAL, NULL, strtoll(arg1, NULL, 10), &tr);
            print_result("withdrawal", st, &tr);
        } else if (strcmp(command, "transfer") == 0 && words == 3) {
            st = session_transaction(&s, TRANSACTION_TRANSFER, arg1, strtoll(arg2, NULL, 10), &tr);
            print_result("transfer", st, &tr);
        } else if (strcmp(command, "check") == 0) {
            st = session_server_check(&s);
            printf("server totals: %s\n", st == STATUS_OK ? "consistent" : status_string(st));
        } else {
            printf("not understood\n");
            continue;
        }

        if (st == STATUS_SERVER_UNAVAILABLE || st == STATUS_SHUTTING_DOWN) {
            printf("connection to the server is gone\n");
            break;
        }
    }

    session_close(&s);
    return 0;
}

/*
 * Automatic client used for the throughput tests.
 * Sends "count" random deposits, withdrawals and transfers and exits
 * with the number the server committed. The seed makes each client's
 * sequence repeatable, so Linux and Windows can run the same workload.
 */
int client_auto(const char *client_id, unsigned long count, unsigned seed, HANDLE stop) {
    client_session_t s;
    unsigned long committed = 0;

    status_t st = session_open(&s, client_id, stop);
    if (st != STATUS_OK) {
        fprintf(stderr, "client %s: cannot log in: %s\n", client_id, status_string(st));
        return -1;
    }

    srand(seed);
    for (unsigned long i = 0; i < count; i++) {
        transaction_response_t tr;
        money_t amount = (money_t)(rand() % 1000) + 1;
        int kind = rand() % 3;

        if (kind == 0) {
            st = session_transaction(&s, TRANSACTION_DEPOSIT, NULL, amount, &tr);
        } else if (kind == 1) {
            st = session_transaction(&s, TRANSACTION_WITHDRAWAL, NULL, amount, &tr);
        } else {
            /* pick one of the other accounts */
            char target[CLIENT_ID_LENGTH + 1];
            unsigned mine = (unsigned)atoi(client_id + 3);
            unsigned other = (mine - 1U + 1U + (unsigned)(rand() % (ACCOUNT_COUNT - 1U))) % ACCOUNT_COUNT + 1U;
            snprintf(target, sizeof target, "ACC%04u", other);
            st = session_transaction(&s, TRANSACTION_TRANSFER, target, amount, &tr);
        }

        if (st == STATUS_OK) {
            committed++;
        } else if (st == STATUS_SERVER_UNAVAILABLE || st == STATUS_SHUTTING_DOWN ||
                   st == STATUS_QUEUE_ERROR) {
            break;                                  /* server gone or Ctrl+C */
        }
        /* a refusal such as insufficient funds is a normal answer, carry on */
    }

    session_close(&s);
    return (int)committed;
}
