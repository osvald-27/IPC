
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <ctype.h>

/* Use the shared types defined by the common folder. */
#include "../common/types.h"

/* Maximum number of characters a command line can contain. */
#define COMMAND_LENGTH 100U

/* Different commands the client can enter. */
typedef enum {
    CLI_BALANCE,
    CLI_DEPOSIT,
    CLI_WITHDRAW,
    CLI_TRANSFER,
    CLI_LOGOUT,
    CLI_HELP,
    CLI_INVALID
} cli_command_type_t;

/* Stores the command entered by the user. */
typedef struct {
    cli_command_type_t type;
    client_id_t receiver_id;
    money_t amount;
} cli_command_t;


/* Converts an amount entered as text into money_t.
   Returns 1 if valid and 0 if invalid. */
static int parse_amount(const char *text, money_t *amount)
{
    char *end;
    intmax_t value;

    errno = 0;

    /* Convert the text into an integer. */
    value = strtoimax(text, &end, 10);

    /* Reject invalid numbers, overflow and non-positive amounts. */
    if (text == end || *end != '\0' ||
        errno == ERANGE || value <= 0 || value > INT64_MAX) {
        return 0;
    }

    *amount = (money_t)value;

    return 1;
}


/* Reads one command from the user and identifies what it means.

   Return values:
   1  = A valid command was entered.
   0  = The user reached the end of input.
  -1  = The command is invalid. */
int cli_read_command(cli_command_t *command)
{
    char input[COMMAND_LENGTH];
    char operation[20];
    char receiver[CLIENT_ID_LENGTH + 1U];
    char amount_text[32];
    char extra;
    int i;

    /* Make sure the command pointer is valid. */
    if (command == NULL) {
        return -1;
    }

    /* Start with an empty command. */
    memset(command, 0, sizeof(*command));
    command->type = CLI_INVALID;

    printf("crossipc> ");
    fflush(stdout);

    /* Read the command entered by the user. */
    if (fgets(input, sizeof(input), stdin) == NULL) {
        return 0;
    }

    /* Reject lines that are too long. */
    if (strchr(input, '\n') == NULL && !feof(stdin)) {
        int ch;

        while ((ch = getchar()) != '\n' && ch != EOF) {
            /* Discard the remaining characters. */
        }

        return -1;
    }

    /* Get the command name. */
    if (sscanf(input, "%19s", operation) != 1) {
        return -1;
    }


    /* BALANCE COMMAND */
    if (strcmp(operation, "balance") == 0) {

        /* Balance does not require any extra argument. */
        if (sscanf(input, "%19s %c", operation, &extra) != 1) {
            return -1;
        }

        command->type = CLI_BALANCE;
    }


    /* DEPOSIT OR WITHDRAW COMMAND */
    else if (strcmp(operation, "deposit") == 0 ||
             strcmp(operation, "withdraw") == 0) {

        /* Read the operation and amount. */
        if (sscanf(input, "%19s %31s %c",
                   operation, amount_text, &extra) != 2) {
            return -1;
        }

        /* Check that the amount is valid. */
        if (!parse_amount(amount_text, &command->amount)) {
            return -1;
        }

        /* Identify which operation the user requested. */
        if (strcmp(operation, "deposit") == 0) {
            command->type = CLI_DEPOSIT;
        } else {
            command->type = CLI_WITHDRAW;
        }
    }


    /* TRANSFER COMMAND */
    else if (strcmp(operation, "transfer") == 0) {

        /* Read the receiver ID and transfer amount. */
        if (sscanf(input, "%19s %7s %31s %c",
                   operation, receiver, amount_text, &extra) != 3) {
            return -1;
        }

        /* Check that the receiver ID has the required length. */
        if (strlen(receiver) != CLIENT_ID_LENGTH) {
            return -1;
        }

        /* Check that the receiver ID contains valid characters. */
        for (i = 0; receiver[i] != '\0'; i++) {
            if (!isalnum((unsigned char)receiver[i])) {
                return -1;
            }
        }

        /* Check that the transfer amount is valid. */
        if (!parse_amount(amount_text, &command->amount)) {
            return -1;
        }

        /* Store the receiver ID in the shared client ID structure. */
        strcpy(command->receiver_id.value, receiver);

        command->type = CLI_TRANSFER;
    }


    /* LOGOUT COMMAND */
    else if (strcmp(operation, "logout") == 0) {

        /* Logout must not have additional arguments. */
        if (sscanf(input, "%19s %c", operation, &extra) != 1) {
            return -1;
        }

        command->type = CLI_LOGOUT;
    }


    /* HELP COMMAND */
    else if (strcmp(operation, "help") == 0) {

        /* Help must not have additional arguments. */
        if (sscanf(input, "%19s %c", operation, &extra) != 1) {
            return -1;
        }

        command->type = CLI_HELP;
    }


    /* UNKNOWN COMMAND */
    else {
        return -1;
    }

    return 1;
}


/* Displays the commands available to the user. */
void cli_show_help(void)
{
    printf("\nAvailable commands:\n");

    printf("  balance                  Check account balance\n");
    printf("  deposit <amount>         Deposit money\n");
    printf("  withdraw <amount>        Withdraw money\n");
    printf("  transfer <ID> <amount>   Transfer money\n");
    printf("  logout                   End your session\n");
    printf("  help                     Show available commands\n");

    printf("\n");
}
