#include "server.h"

#include <stdio.h>
#include <string.h>

#include "../named_pipe/named_pipe.h"

/*
 * What a worker process does: wait for a client on its pipe instance,
 * serve that client's requests until it logs out or disconnects, then
 * wait for the next one.
 */

/* Every reply is a response_t, optionally followed by one more struct. */
static status_t reply(server_ctx_t *ctx, pipe_conn_t *conn, const message_header_t *request,
                      const session_t *session, status_t status, const void *extra, uint32_t extra_size) {
    char payload[sizeof(response_t) + sizeof(transaction_response_t) + sizeof(balance_response_t)];
    response_t r;

    r.status = status;
    r.payload_size = extra_size;
    memcpy(payload, &r, sizeof r);
    if (extra_size > 0) {
        memcpy(payload + sizeof r, extra, extra_size);
    }

    message_header_t header;
    memset(&header, 0, sizeof header);
    header.magic = PROTOCOL_NUMBER;
    header.version = PROTOCOL_VERSION;
    header.header_size = sizeof header;
    header.message_type = RESPONSE_MESSAGE;
    header.payload_size = (uint32_t)sizeof r + extra_size;
    header.request_id = request->request_id;     /* lets the client match reply to request */
    header.session_id = session->session_id;

    return pipe_send(conn, &header, payload, ctx->stop);
}

static int same_client(const client_id_t *a, const client_id_t *b) {
    return strncmp(a->value, b->value, CLIENT_ID_LENGTH) == 0;
}

/* Returns 0 when the conversation with this client is over. */
static int handle_request(server_ctx_t *ctx, pipe_conn_t *conn, session_t *session,
                          const message_header_t *header, const char *payload) {
    status_t st;

    if (header->message_type == REQUEST_LOGIN) {
        if (header->payload_size != sizeof(login_request_t)) {
            return reply(ctx, conn, header, session, STATUS_INVALID_ARGUMENT, NULL, 0) == STATUS_OK;
        }
        login_request_t login;
        memcpy(&login, payload, sizeof login);
        st = auth_login(ctx, session, &login);
        return reply(ctx, conn, header, session, st, NULL, 0) == STATUS_OK;
    }

    /* everything else needs a valid session */
    st = auth_check(session, header);
    if (st != STATUS_OK) {
        return reply(ctx, conn, header, session, st, NULL, 0) == STATUS_OK;
    }

    switch (header->message_type) {
    case REQUEST_DEPOSIT:
    case REQUEST_WITHDRAWAL:
    case REQUEST_TRANSFER: {
        transaction_request_t request;
        transaction_response_t response;

        if (header->payload_size != sizeof request) {
            return reply(ctx, conn, header, session, STATUS_INVALID_ARGUMENT, NULL, 0) == STATUS_OK;
        }
        memcpy(&request, payload, sizeof request);
        request.sender_id.value[CLIENT_ID_LENGTH] = '\0';
        request.receiver_id.value[CLIENT_ID_LENGTH] = '\0';

        /* a client may only move money out of its own account */
        if (!same_client(&request.sender_id, &session->client_id)) {
            return reply(ctx, conn, header, session, STATUS_PERMISSION_DENIED, NULL, 0) == STATUS_OK;
        }
        /* the message type and the type inside the payload must agree */
        if ((header->message_type == REQUEST_DEPOSIT && request.type != TRANSACTION_DEPOSIT) ||
            (header->message_type == REQUEST_WITHDRAWAL && request.type != TRANSACTION_WITHDRAWAL) ||
            (header->message_type == REQUEST_TRANSFER && request.type != TRANSACTION_TRANSFER)) {
            return reply(ctx, conn, header, session, STATUS_INVALID_ARGUMENT, NULL, 0) == STATUS_OK;
        }

        st = transaction_execute(ctx, &request, &response);
        return reply(ctx, conn, header, session, st, &response, sizeof response) == STATUS_OK;
    }

    case REQUEST_GET_BALANCE: {
        balance_response_t balance;
        st = transaction_balance(ctx, &session->client_id, &balance);
        return reply(ctx, conn, header, session, st, &balance, sizeof balance) == STATUS_OK;
    }

    case REQUEST_SERVER_STATUS: {
        /* STATUS_OK means the consistency check passed */
        int ok;
        if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
            return reply(ctx, conn, header, session, STATUS_SYNCHRONIZATION_ERROR, NULL, 0) == STATUS_OK;
        }
        ok = registry_consistent(ctx->bank, NULL, NULL);
        if (ctx->use_lock) {
            semaphore_unlock(ctx->lock);
        }
        return reply(ctx, conn, header, session, ok ? STATUS_OK : STATUS_INTERNAL_ERROR, NULL, 0) == STATUS_OK;
    }

    case REQUEST_LOGOUT:
        reply(ctx, conn, header, session, STATUS_OK, NULL, 0);
        auth_logout(session);
        return 0;

    default:
        return reply(ctx, conn, header, session, STATUS_INVALID_ARGUMENT, NULL, 0) == STATUS_OK;
    }
}

static void serve_client(server_ctx_t *ctx, pipe_conn_t *conn) {
    session_t session;
    message_header_t header;
    char payload[MAX_PAYLOAD_SIZE];

    memset(&session, 0, sizeof session);
    for (;;) {
        status_t st = pipe_recv(conn, &header, payload, sizeof payload, ctx->stop);
        if (st != STATUS_OK) {
            /* STATUS_QUEUE_EMPTY: the client closed its end, also when it crashed */
            if (st != STATUS_QUEUE_EMPTY && st != STATUS_SHUTTING_DOWN) {
                fprintf(stderr, "worker %d: bad message from client, dropping it\n", ctx->worker_id);
            }
            break;
        }
        if (!handle_request(ctx, conn, &session, &header, payload)) {
            break;
        }
    }
    auth_logout(&session);
}

int worker_run(server_ctx_t *ctx) {
    pipe_conn_t *conn = NULL;

    if (pipe_server_create(&conn) != STATUS_OK) {
        return 1;
    }

    while (WaitForSingleObject(ctx->stop, 0) != WAIT_OBJECT_0) {
        status_t st = pipe_server_accept(conn, ctx->stop);
        if (st == STATUS_SHUTTING_DOWN) {
            break;
        }
        if (st == STATUS_OK) {
            serve_client(ctx, conn);
        }
        pipe_server_disconnect(conn);
    }

    pipe_close(conn);
    return 0;
}
