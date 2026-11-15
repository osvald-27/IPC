#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <ctype.h>

#include "../common/types.h"

#define COMMAND_LENGTH 100U

typedef enum cli_command_type {
    CLI_BALANCE,
    CLI_DEPOSIT,
    CLI_WITHDRAW,
    CLI_TRANSFER,
    CLI_LOGOUT,
    CLI_HELP,
    CLI_INVALID
} cli_command_type_t;

typedef struct cli_command {
    cli_command_type_t type;
    client_id_t receiver_id;
    money_t amount;
} cli_command_t;

static int parse_amount(const char *text, money_t *amount)
{
    char *end;
    intmax_t value;

    errno = 0;
    value = strtoimax(text, &end, 10);

    if (text == end || *end != '\0' ||
        errno == ERANGE || value <= 0) {
        return 0;
    }

    *amount = (money_t)value;
    return 1;
}

int cli_read_command(cli_command_t *command)
{
    char input[COMMAND_LENGTH];
    char operation[20];
    char receiver[CLIENT_ID_LENGTH + 1U];
    char amount_text[32];
    char extra;
    size_t i;

    if (command == NULL) {
        return -1;
    }

    memset(command, 0, sizeof(*command));
    command->type = CLI_INVALID;

    printf("crossipc> ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL) {
        return 0;
    }

    if (strchr(input, '\n') == NULL && !feof(stdin)) {
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {
        }
        return -1;
    }

    if (sscanf(input, "%19s", operation) != 1) {
        return -1;
    }

    if (strcmp(operation, "balance") == 0) {
        if (sscanf(input, "%19s %c", operation, &extra) != 1) {
            return -1;
        }
        command->type = CLI_BALANCE;
    }

    else if (strcmp(operation, "deposit") == 0 ||
             strcmp(operation, "withdraw") == 0) {

        if (sscanf(input, "%19s %31s %c",
                   operation, amount_text, &extra) != 2) {
            return -1;
        }

        if (!parse_amount(amount_text, &command->amount)) {
            return -1;
        }

        if (strcmp(operation, "deposit") == 0) {
            command->type = CLI_DEPOSIT;
        } else {
            command->type = CLI_WITHDRAW;
        }
    }

    else if (strcmp(operation, "transfer") == 0) {

        if (sscanf(input, "%19s %7s %31s %c",
                   operation, receiver, amount_text, &extra) != 3) {
            return -1;
        }

        if (strlen(receiver) != CLIENT_ID_LENGTH) {
            return -1;
        }

        for (i = 0; receiver[i] != '\0'; i++) {
            if (!isalnum((unsigned char)receiver[i])) {
                return -1;
            }
        }

        if (!parse_amount(amount_text, &command->amount)) {
            return -1;
        }

        strcpy(command->receiver_id.value, receiver);
        command->type = CLI_TRANSFER;
    }

    else if (strcmp(operation, "logout") == 0) {
        if (sscanf(input, "%19s %c", operation, &extra) != 1) {
            return -1;
        }
        command->type = CLI_LOGOUT;
    }

    else if (strcmp(operation, "help") == 0) {
        if (sscanf(input, "%19s %c", operation, &extra) != 1) {
            return -1;
        }
        command->type = CLI_HELP;
    }

    else {
        return -1;
    }

    return 1;
}

void cli_show_help(void)
{
    printf("\nAvailable commands:\n");
    printf("  balance                  Check account balance\n");
    printf("  deposit <amount>         Deposit money\n");
    printf("  withdraw <amount>        Withdraw money\n");
    printf("  transfer <ID> <amount>   Transfer money\n");
    printf("  logout                   End your session\n");
    printf("  help                     Show available commands\n\n");
}
