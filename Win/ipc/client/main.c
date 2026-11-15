
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>

#include "../common/types.h"
#include "../common/protocol.h"
#include "../common/status.h"
#include "../named_pipe/named_pipe.h"

/* These definitions must match cli.c and session.c. */

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

typedef struct session {
    client_id_t client_id;
    session_id_t session_id;
    int authenticated;
} session_t;

/* Functions implemented in cli.c. */
int cli_read_command(cli_command_t *command);
void cli_show_help(void);

/* Functions implemented in session.c. */
void session_init(session_t *session);
int session_start(session_t *session,
                  const client_id_t *client_id,
                  const session_id_t *session_id);
void session_end(session_t *session);
int session_is_authenticated(const session_t *session);

/* Generate a different request ID for each request. */
static uint32_t request_sequence = 0;

/*
 * Send a request and receive the server response.
 * The response payload is copied into response_data.
 */
static status_t exchange_request(
    named_pipe_t *pipe,
    uint32_t message_type,
    const session_id_t *session_id,
    const void *request_payload,
    uint32_t request_size,
    response_t *server_response,
    void *response_data,
    uint32_t response_capacity,
    uint32_t *response_data_size)
{
    message_header_t request_header;
    message_header_t response_header;
    uint8_t received[MAX_PAYLOAD_SIZE];

    status_t result;

    if (pipe == NULL || server_response == NULL ||
        response_data_size == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(&request_header, 0, sizeof(request_header));
    memset(&response_header, 0, sizeof(response_header));

    request_header.message_type = message_type;
    request_header.payload_size = request_size;

    request_sequence++;

    snprintf(request_header.request_id.value,
             sizeof(request_header.request_id.value),
             "REQ-%08" PRIu32,
             request_sequence);

    if (session_id != NULL) {
        request_header.session_id = *session_id;
    }

    result = pipe_send_message(
        pipe,
        &request_header,
        request_payload
    );

    if (result != STATUS_OK) {
        return result;
    }

    result = pipe_recv_message(
        pipe,
        &response_header,
        received,
        MAX_PAYLOAD_SIZE
    );

    if (result != STATUS_OK) {
        return result;
    }

    if (response_header.message_type != RESPONSE_MESSAGE ||
          strcmp(response_header.request_id.value,
           request_header.request_id.value) != 0 ||
        response_header.payload_size < sizeof(response_t) ||
        response_header.payload_size > MAX_PAYLOAD_SIZE) {
        return STATUS_ERROR;
    }

    memcpy(server_response, received, sizeof(response_t));

    if (server_response->payload_size >
        response_header.payload_size - sizeof(response_t)) {
        return STATUS_ERROR;
    }

    if (server_response->payload_size > response_capacity) {
        return STATUS_ERROR;
    }

    if (server_response->payload_size > 0U) {
        if (response_data == NULL) {
            return STATUS_INVALID_ARGUMENT;
        }

        memcpy(
            response_data,
            received + sizeof(response_t),
            server_response->payload_size
        );
    }

    *response_data_size = server_response->payload_size;

    return STATUS_OK;
}

/* Read and validate the client ID. */
static int read_client_id(client_id_t *client_id)
{
    char input[64];
    size_t length;
    size_t i;

    if (client_id == NULL) {
        return 0;
    }

    printf("Enter your client ID: ");
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL) {
        return 0;
    }

    if (strchr(input, '\n') == NULL && !feof(stdin)) {
        int ch;

        while ((ch = getchar()) != '\n' && ch != EOF) {
        }

        printf("Client ID is too long.\n");
        return 0;
    }

    input[strcspn(input, "\r\n")] = '\0';
    length = strlen(input);

    if (length != CLIENT_ID_LENGTH) {
        printf("Client ID must contain exactly %u characters.\n",
               (unsigned int)CLIENT_ID_LENGTH);
        return 0;
    }

    for (i = 0; i < length; i++) {
        if (!isalnum((unsigned char)input[i])) {
            printf("Client ID must contain only letters and numbers.\n");
            return 0;
        }
    }

    memset(client_id, 0, sizeof(*client_id));
    strcpy(client_id->value, input);

    return 1;
}

/* Log in and obtain the session ID from the server. */
static int client_login(
    named_pipe_t *pipe,
    session_t *session,
    const client_id_t *client_id)
{
    login_request_t login_request;
    session_id_t issued_session;
    response_t response;
    uint32_t response_size = 0;
    status_t result;

    memset(&login_request, 0, sizeof(login_request));
    memset(&issued_session, 0, sizeof(issued_session));

    login_request.client_id = *client_id;

    result = exchange_request(
        pipe,
        REQUEST_LOGIN,
        NULL,
        &login_request,
        sizeof(login_request),
        &response,
        &issued_session,
        sizeof(issued_session),
        &response_size
    );

    if (result != STATUS_OK) {
        printf("Login communication failed. Status: %d\n", (int)result);
        return 0;
    }

    if (response.status != STATUS_OK) {
        printf("Login failed. Server status: %d\n",
               (int)response.status);
        return 0;
    }

    if (response_size != sizeof(session_id_t) ||
        issued_session.value[0] == '\0') {
        printf("Invalid login response from server.\n");
        return 0;
    }

    if (!session_start(session, client_id, &issued_session)) {
        printf("Could not establish client session.\n");
        return 0;
    }

    printf("Login successful. Welcome, %s!\n", client_id->value);

    return 1;
}

/* Request and display the account balance. */
static void client_get_balance(
    named_pipe_t *pipe,
    const session_t *session)
{
    balance_request_t request;
    balance_response_t balance;
    response_t response;
    uint32_t response_size = 0;
    status_t result;

    memset(&request, 0, sizeof(request));
    memset(&balance, 0, sizeof(balance));

    request.client_id = session->client_id;

    result = exchange_request(
        pipe,
        REQUEST_GET_BALANCE,
        &session->session_id,
        &request,
        sizeof(request),
        &response,
        &balance,
        sizeof(balance),
        &response_size
    );

    if (result != STATUS_OK) {
        printf("Balance request failed. Communication status: %d\n",
               (int)result);
        return;
    }

    if (response.status != STATUS_OK) {
        printf("Balance request rejected. Server status: %d\n",
               (int)response.status);
        return;
    }

    if (response_size != sizeof(balance_response_t)) {
        printf("Invalid balance response from server.\n");
        return;
    }

    printf("\nAccount: %s\n", balance.client_id.value);
    printf("Balance: %" PRId64 " XAF\n", balance.balance);
}

/* Send a deposit, withdrawal, or transfer request. */
static void client_transaction(
    named_pipe_t *pipe,
    const session_t *session,
    const cli_command_t *command)
{
    transaction_request_t request;
    transaction_response_t transaction_response;
    response_t response;
    uint32_t message_type;
    uint32_t response_size = 0;
    status_t result;

    memset(&request, 0, sizeof(request));
    memset(&transaction_response, 0, sizeof(transaction_response));

    request.sender_id = session->client_id;
    request.amount = command->amount;

    switch (command->type) {
        case CLI_DEPOSIT:
            message_type = REQUEST_DEPOSIT;
            request.type = TRANSACTION_DEPOSIT;
            break;

        case CLI_WITHDRAW:
            message_type = REQUEST_WITHDRAWAL;
            request.type = TRANSACTION_WITHDRAWAL;
            break;

        case CLI_TRANSFER:
            message_type = REQUEST_TRANSFER;
            request.type = TRANSACTION_TRANSFER;
            request.receiver_id = command->receiver_id;
            break;

        default:
            printf("Invalid transaction command.\n");
            return;
    }

    result = exchange_request(
        pipe,
        message_type,
        &session->session_id,
        &request,
        sizeof(request),
        &response,
        &transaction_response,
        sizeof(transaction_response),
        &response_size
    );

    if (result != STATUS_OK) {
        printf("Transaction communication failed. Status: %d\n",
               (int)result);
        return;
    }

    if (response.status != STATUS_OK) {
        printf("Transaction rejected. Server status: %d\n",
               (int)response.status);
        return;
    }

    if (response_size != sizeof(transaction_response_t)) {
        printf("Invalid transaction response from server.\n");
        return;
    }

    printf("\nTransaction response received.\n");
    printf("Transaction status: %d\n",
           (int)transaction_response.status);
    printf("Transaction state: %d\n",
           (int)transaction_response.state);
    printf("Updated balance: %" PRId64 " XAF\n",
           transaction_response.balance);
}

/* Send a logout request to the server. */
static void client_logout(
    named_pipe_t *pipe,
    const session_t *session)
{
    response_t response;
    uint32_t response_size = 0;
    status_t result;

    result = exchange_request(
        pipe,
        REQUEST_LOGOUT,
        &session->session_id,
        NULL,
        0U,
        &response,
        NULL,
        0U,
        &response_size
    );

    if (result != STATUS_OK) {
        printf("Logout communication failed. Status: %d\n",
               (int)result);
        return;
    }

    if (response.status == STATUS_OK) {
        printf("Logout successful.\n");
    } else {
        printf("Logout failed. Server status: %d\n",
               (int)response.status);
    }
}

int main(void)
{
    named_pipe_t *pipe = NULL;
    session_t session;
    client_id_t client_id;
    cli_command_t command;

    status_t result;
    int command_result;

    session_init(&session);

    printf("\nCrossIPC Windows Client\n");
    printf("=======================\n");
    printf("Connecting to server...\n");

    result = pipe_client_open(PIPE_NAME, 5000U, &pipe);

    if (result != STATUS_OK) {
        fprintf(stderr,
                "Connection failed. Status: %d\n",
                (int)result);
        return 1;
    }

    printf("Connected to server successfully.\n");

    if (!read_client_id(&client_id)) {
        printf("Invalid client ID. Closing client.\n");
        goto cleanup;
    }

    if (!client_login(pipe, &session, &client_id)) {
        goto cleanup;
    }

    printf("\nEnter 'help' to see available commands.\n");

    while (session_is_authenticated(&session)) {

        command_result = cli_read_command(&command);

        if (command_result == 0) {
            printf("\nEnd of input.\n");
            break;
        }

        if (command_result == -1) {
            printf("Invalid command. Enter 'help' for assistance.\n");
            continue;
        }

        switch (command.type) {

            case CLI_HELP:
                cli_show_help();
                break;

            case CLI_BALANCE:
                client_get_balance(pipe, &session);
                break;

            case CLI_DEPOSIT:
            case CLI_WITHDRAW:
            case CLI_TRANSFER:
                client_transaction(pipe, &session, &command);
                break;

            case CLI_LOGOUT:
                client_logout(pipe, &session);
                goto cleanup;

            default:
                printf("Unknown command.\n");
                break;
        }
    }

cleanup:
    session_end(&session);

    if (pipe != NULL) {
        pipe_close(pipe);
        pipe = NULL;
    }

    printf("Connection closed.\n");

    return 0;
}
