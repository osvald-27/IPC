#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void handle_request(server_ctx_t *ctx, session_t *session, const Message *request, Message *response) {
    memset(response, 0, sizeof(*response));
    response->mtype = request->sender_pid;
    response->request_id = request->request_id;
    response->sender_pid = (int32_t)getpid();
    response->operation = request->operation;
    snprintf(response->client_id, sizeof(response->client_id), "%.*s", CLIENT_ID_LEN - 1, request->client_id);
    snprintf(response->session_id, sizeof(response->session_id), "%.*s", SESSION_ID_LEN - 1, request->session_id);

    /* 1. Login Request */
    if (request->operation == MSG_LOGIN) {
        status_t st = auth_login(ctx, session, request->client_id, (pid_t)request->sender_pid);
        response->status_code = st;
        if (st == STATUS_OK) {
            snprintf(response->session_id, sizeof(response->session_id), "%.*s", SESSION_ID_LEN - 1, session->session_id.value);
        }
        return;
    }

    /* 2. Verify session token against shared memory across all worker processes */
    status_t auth_st = auth_check(ctx, request->client_id, request->session_id);
    if (auth_st != STATUS_OK) {
        response->status_code = auth_st;
        return;
    }

    /* 3. Handle Authenticated Operations */
    switch (request->operation) {
    case MSG_BALANCE: {
        balance_response_t bal;
        client_id_t cid;
        memset(&cid, 0, sizeof(cid));
        snprintf(cid.value, sizeof(cid.value), "%.*s", (int)CLIENT_ID_LENGTH, request->client_id);

        status_t st = transaction_balance(ctx, &cid, &bal);
        response->status_code = st;
        if (st == STATUS_OK) {
            response->amount = bal.balance;
        }
        break;
    }

    case MSG_DEPOSIT:
    case MSG_WITHDRAW:
    case MSG_TRANSFER: {
        transaction_execute(ctx, request, response);
        break;
    }

    case MSG_STATUS_QUERY: {
        transaction_status_query(ctx, request->request_id, response);
        break;
    }

    case MSG_SERVER_STATUS: {
        int consistent = 0;
        if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
            response->status_code = STATUS_SYNCHRONIZATION_ERROR;
            break;
        }
        consistent = registry_consistent(ctx->bank, NULL, NULL);
        if (ctx->use_lock) {
            semaphore_unlock(ctx->lock);
        }
        response->status_code = consistent ? STATUS_OK : STATUS_INTERNAL_ERROR;
        break;
    }

    case MSG_LOGOUT: {
        auth_logout(ctx, request->session_id);
        response->status_code = STATUS_OK;
        break;
    }

    default:
        response->status_code = STATUS_INVALID_ARGUMENT;
        break;
    }
}

int server_msg_send(int qid, const Message *msg) {
    if (msg == NULL) return -1;
    size_t payload_size = sizeof(Message) - sizeof(long);
    return msgsnd(qid, msg, payload_size, 0);
}

int server_msg_recv(int qid, Message *msg, long mtype) {
    if (msg == NULL) return -1;
    size_t payload_size = sizeof(Message) - sizeof(long);
    return msgrcv(qid, msg, payload_size, mtype, 0) == -1 ? -1 : 0;
}

int worker_run(server_ctx_t *ctx) {
    if (ctx == NULL) {
        return 1;
    }

    session_t session;
    memset(&session, 0, sizeof(session));

    Message request;
    Message response;

    while (!ctx->stop) {
        if (server_msg_recv(ctx->msg_qid, &request, MSG_REQUEST_TYPE) != 0) {
            if (errno == EINTR || ctx->stop) {
                break;
            }
            continue;
        }

        handle_request(ctx, &session, &request, &response);

        if (response.mtype > 0) {
            server_msg_send(ctx->msg_qid, &response);
        }
    }

    return 0;
}
