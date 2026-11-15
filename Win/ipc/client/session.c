#include "client.h"

#include <stdio.h>
#include <string.h>

static void copy_id(client_id_t *out, const char *text) {
    memset(out, 0, sizeof *out);
    if (text != NULL) {
        strncpy(out->value, text, CLIENT_ID_LENGTH);
    }
}

/*
 * Send one request and wait for its reply.
 * The reply payload is a response_t followed by "extra" (may be empty).
 * Returns the status the server put in response_t, or a local error.
 */
static status_t exchange(client_session_t *s, uint32_t type, const void *payload, uint32_t size,
                         void *extra, uint32_t extra_size) {
    message_header_t header;
    char buffer[MAX_PAYLOAD_SIZE];

    memset(&header, 0, sizeof header);
    header.magic = PROTOCOL_NUMBER;
    header.version = PROTOCOL_VERSION;
    header.header_size = sizeof header;
    header.message_type = type;
    header.payload_size = size;
    header.session_id = s->session_id;
    /* process id + counter: unique, so a reply can be matched to its request */
    snprintf(header.request_id.value, sizeof header.request_id.value, "R%lu-%lu",
             (unsigned long)GetCurrentProcessId(), ++s->next_request);

    request_id_t sent = header.request_id;

    status_t st = pipe_send(s->conn, &header, payload, s->stop);
    if (st != STATUS_OK) {
        return st == STATUS_QUEUE_EMPTY ? STATUS_SERVER_UNAVAILABLE : st;
    }

    st = pipe_recv(s->conn, &header, buffer, sizeof buffer, s->stop);
    if (st != STATUS_OK) {
        return st == STATUS_QUEUE_EMPTY ? STATUS_SERVER_UNAVAILABLE : st;
    }
    if (header.message_type != RESPONSE_MESSAGE || header.payload_size < sizeof(response_t) ||
        strncmp(header.request_id.value, sent.value, MAX_REQUEST_ID) != 0) {
        return STATUS_QUEUE_ERROR;
    }

    response_t response;
    memcpy(&response, buffer, sizeof response);

    if (type == REQUEST_LOGIN && response.status == STATUS_OK) {
        s->session_id = header.session_id;      /* the server issued it */
    }
    if (extra != NULL && response.payload_size == extra_size &&
        header.payload_size == sizeof response + extra_size) {
        memcpy(extra, buffer + sizeof response, extra_size);
    }
    return response.status;
}

status_t session_open(client_session_t *s, const char *client_id, HANDLE stop) {
    memset(s, 0, sizeof *s);
    s->stop = stop;
    copy_id(&s->client_id, client_id);

    status_t st = pipe_client_connect(&s->conn, CONNECT_TIMEOUT_MS);
    if (st != STATUS_OK) {
        return st;
    }

    login_request_t login;
    login.client_id = s->client_id;
    st = exchange(s, REQUEST_LOGIN, &login, sizeof login, NULL, 0);
    if (st != STATUS_OK) {
        pipe_close(s->conn);
        s->conn = NULL;
    }
    return st;
}

status_t session_transaction(client_session_t *s, transaction_type_t type, const char *receiver_id,
                             money_t amount, transaction_response_t *response) {
    transaction_request_t request;
    uint32_t message;

    memset(&request, 0, sizeof request);
    request.type = type;
    request.sender_id = s->client_id;
    copy_id(&request.receiver_id, receiver_id);
    request.amount = amount;

    switch (type) {
    case TRANSACTION_DEPOSIT:    message = REQUEST_DEPOSIT;    break;
    case TRANSACTION_WITHDRAWAL: message = REQUEST_WITHDRAWAL; break;
    case TRANSACTION_TRANSFER:   message = REQUEST_TRANSFER;   break;
    default:                     return STATUS_INVALID_ARGUMENT;
    }

    memset(response, 0, sizeof *response);
    return exchange(s, message, &request, sizeof request, response, sizeof *response);
}

status_t session_balance(client_session_t *s, balance_response_t *response) {
    balance_request_t request;
    request.client_id = s->client_id;

    memset(response, 0, sizeof *response);
    return exchange(s, REQUEST_GET_BALANCE, &request, sizeof request, response, sizeof *response);
}

/* STATUS_OK when the server's consistency check passes. */
status_t session_server_check(client_session_t *s) {
    return exchange(s, REQUEST_SERVER_STATUS, NULL, 0, NULL, 0);
}

void session_close(client_session_t *s) {
    if (s->conn == NULL) {
        return;
    }
    /* tell the server we are leaving, unless Ctrl+C already cut us short */
    if (s->stop == NULL || WaitForSingleObject(s->stop, 0) != WAIT_OBJECT_0) {
        exchange(s, REQUEST_LOGOUT, NULL, 0, NULL, 0);
    }
    pipe_close(s->conn);
    s->conn = NULL;
}
