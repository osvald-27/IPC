#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dispatches and executes an incoming client request */
void server_handle_request(server_ctx_t *ctx, session_t *session, const Message *request, Message *response) {
    memset(response, 0, sizeof(*response));
    response->request_id = request->request_id;
    response->sender_pid = (uint32_t)GetCurrentProcessId();
    response->operation = request->operation;
    strncpy(response->client_id, request->client_id, CLIENT_ID_LEN - 1);
    strncpy(response->session_id, session->session_id.value, SESSION_ID_LEN - 1);

    /* 1. Login Request */
    if (request->operation == MSG_LOGIN) {
        status_t st = auth_login(ctx, session, request->client_id, request->sender_pid);
        response->status_code = st;
        if (st == STATUS_OK) {
            strncpy(response->session_id, session->session_id.value, SESSION_ID_LEN - 1);
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

/* Helper to guarantee reading an exact Message struct from the Named Pipe */
static int pipe_read_exact(HANDLE pipe, void *buf, DWORD len) {
    DWORD total = 0;
    char *ptr = (char *)buf;
    while (total < len) {
        DWORD read_now = 0;
        if (!ReadFile(pipe, ptr + total, len - total, &read_now, NULL) || read_now == 0) {
            return -1;
        }
        total += read_now;
    }
    return 0;
}

/* Helper to guarantee writing an exact Message struct to the Named Pipe */
static int pipe_write_exact(HANDLE pipe, const void *buf, DWORD len) {
    DWORD total = 0;
    const char *ptr = (const char *)buf;
    while (total < len) {
        DWORD written_now = 0;
        if (!WriteFile(pipe, ptr + total, len - total, &written_now, NULL) || written_now == 0) {
            return -1;
        }
        total += written_now;
    }
    FlushFileBuffers(pipe);
    return 0;
}

/* Worker thread loop: accepts client pipe connections and serves requests */
static DWORD WINAPI worker_thread_func(LPVOID arg) {
    server_ctx_t *ctx = (server_ctx_t *)arg;

    while (!ctx->stop) {
        HANDLE pipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(Message),
            sizeof(Message),
            0,
            NULL
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "[WORKER %d] CreateNamedPipeA failed (error %lu)\n",
                    ctx->worker_id, GetLastError());
            Sleep(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected || ctx->stop) {
            CloseHandle(pipe);
            continue;
        }

        session_t session;
        memset(&session, 0, sizeof(session));

        Message request;
        Message response;

        while (!ctx->stop) {
            if (pipe_read_exact(pipe, &request, sizeof(Message)) != 0) {
                break; /* Client disconnected or pipe closed */
            }

            server_handle_request(ctx, &session, &request, &response);

            if (pipe_write_exact(pipe, &response, sizeof(Message)) != 0) {
                break;
            }
        }

        auth_logout(&session);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    free(ctx);
    return 0;
}

int server_run(server_ctx_t *ctx) {
    if (ctx == NULL) {
        return 1;
    }
    server_ctx_t *copy = malloc(sizeof(server_ctx_t));
    if (copy == NULL) {
        return 1;
    }
    *copy = *ctx;
    worker_thread_func(copy);
    return 0;
}
