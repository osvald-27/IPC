#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Dispatches and executes an incoming client request */
static void handle_request(server_ctx_t *ctx, session_t *session, const Message *request, Message *response) {
    memset(response, 0, sizeof(*response));
    response->mtype = request->sender_pid;  /* Route response to client's PID */
    response->request_id = request->request_id;
    response->sender_pid = (int32_t)getpid();
    response->operation = request->operation;
    strncpy(response->client_id, request->client_id, CLIENT_ID_LEN);
    strncpy(response->session_id, session->session_id.value, SESSION_ID_LEN);

    /* 1. Login Request */
    if (request->operation == MSG_LOGIN) {
        status_t st = auth_login(ctx, session, request->client_id, (pid_t)request->sender_pid);
        response->status_code = st;
        if (st == STATUS_OK) {
            strncpy(response->session_id, session->session_id.value, SESSION_ID_LEN);
        }
        return;
    }

    /* 2. For all subsequent requests, verify session token */
    status_t auth_st = auth_check(session, request->client_id, request->session_id);
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
        strncpy(cid.value, session->client_id.value, CLIENT_ID_LENGTH);

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
        /* Clients can only operate or initiate debit on their own authenticated account */
        if (strncmp(request->client_id, session->client_id.value, CLIENT_ID_LENGTH) != 0) {
            response->status_code = STATUS_PERMISSION_DENIED;
            break;
        }
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
        auth_logout(session);
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

/* Worker loop: continuously retrieves and processes client requests */
int worker_run(server_ctx_t *ctx) {
    if (ctx == NULL) {
        return 1;
    }

    session_t session;
    memset(&session, 0, sizeof(session));

    Message request;
    Message response;

    while (!ctx->stop) {
        /* Block until a request arrives from any client on MSG_REQUEST_TYPE (1L) */
        if (server_msg_recv(ctx->msg_qid, &request, MSG_REQUEST_TYPE) != 0) {
            if (errno == EINTR || ctx->stop) {
                break;
            }
            continue;
        }

        /* Process request */
        handle_request(ctx, &session, &request, &response);

        /* Send response back to the client using its PID as the message type */
        if (response.mtype > 0) {
            server_msg_send(ctx->msg_qid, &response);
        }
    }

    auth_logout(&session);
    return 0;
}
