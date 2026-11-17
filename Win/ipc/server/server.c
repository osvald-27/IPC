#include "server.h"

#include "../named_pipe/named_pipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_field(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

void server_handle_request(
    server_ctx_t *ctx,
    session_t *session,
    const Message *request,
    Message *response
)
{
    status_t status;

    if (ctx == NULL || session == NULL || request == NULL || response == NULL) {
        return;
    }

    memset(response, 0, sizeof(*response));
    response->request_id = request->request_id;
    response->sender_pid = GetCurrentProcessId();
    response->operation = request->operation;

    copy_field(response->client_id, sizeof(response->client_id), request->client_id);

    if (request->operation == MSG_LOGIN) {
        status = auth_login(
            ctx,
            session,
            request->client_id,
            request->sender_pid
        );

        response->status_code = status;

        if (status == STATUS_OK) {
            copy_field(
                response->session_id,
                sizeof(response->session_id),
                session->session_id.value
            );
        }

        return;
    }

    if (request->operation == MSG_ACCOUNT_CREATE) {
        response->status_code = STATUS_ALREADY_EXISTS;
        return;
    }

    status = auth_check(
        session,
        request->client_id,
        request->session_id
    );

    if (status != STATUS_OK) {
        response->status_code = status;
        return;
    }

    switch (request->operation) {
        case MSG_BALANCE:
        {
            balance_response_t balance;
            client_id_t client_id;

            memset(&client_id, 0, sizeof(client_id));
            copy_field(
                client_id.value,
                sizeof(client_id.value),
                session->client_id.value
            );

            status = transaction_balance(ctx, &client_id, &balance);
            response->status_code = status;

            if (status == STATUS_OK) {
                response->amount = balance.balance;
            }

            break;
        }

        case MSG_DEPOSIT:
        case MSG_WITHDRAW:
        case MSG_TRANSFER:
        {
            if (strncmp(
                    request->client_id,
                    session->client_id.value,
                    CLIENT_ID_LENGTH
                ) != 0) {
                response->status_code = STATUS_PERMISSION_DENIED;
                break;
            }

            status = transaction_execute(ctx, request, response);
            response->status_code = status;
            break;
        }

        case MSG_STATUS_QUERY:
        {
            copy_field(
                response->client_id,
                sizeof(response->client_id),
                session->client_id.value
            );

            status = transaction_status_query(
                ctx,
                request->request_id,
                response
            );

            if (status == STATUS_OK) {
                response->request_id = request->request_id;
            }

            break;
        }

        case MSG_SERVER_STATUS:
        {
            int consistent;

            if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
                response->status_code = STATUS_SYNCHRONIZATION_ERROR;
                break;
            }

            consistent = registry_consistent(ctx->bank, NULL, NULL);

            if (ctx->use_lock) {
                semaphore_unlock(ctx->lock);
            }

            response->status_code = consistent
                ? STATUS_OK
                : STATUS_INTERNAL_ERROR;

            break;
        }

        case MSG_LOGOUT:
        {
            auth_logout(session);
            response->status_code = STATUS_OK;
            break;
        }

        default:
        {
            response->status_code = STATUS_INVALID_ARGUMENT;
            break;
        }
    }

    if (session->logged_in) {
        copy_field(
            response->session_id,
            sizeof(response->session_id),
            session->session_id.value
        );
    }
}

static DWORD WINAPI worker_thread_func(LPVOID argument)
{
    server_ctx_t *ctx = (server_ctx_t *)argument;

    if (ctx == NULL) {
        return 1;
    }

    while (InterlockedCompareExchange(&ctx->stop, 0, 0) == 0) {
        HANDLE pipe_handle = INVALID_HANDLE_VALUE;
        session_t session;
        status_t status;

        memset(&session, 0, sizeof(session));

        status = named_pipe_server_create(&pipe_handle);

        if (status != STATUS_OK) {
            Sleep(100);
            continue;
        }

        status = named_pipe_server_accept(pipe_handle);

        if (status != STATUS_OK) {
            named_pipe_server_disconnect(pipe_handle);
            if (InterlockedCompareExchange(&ctx->stop, 0, 0) != 0) {
                break;
            }
            continue;
        }

        while (InterlockedCompareExchange(&ctx->stop, 0, 0) == 0) {
            Message request;
            Message response;

            memset(&request, 0, sizeof(request));
            memset(&response, 0, sizeof(response));

            status = named_pipe_read_exact(
                pipe_handle,
                &request,
                sizeof(request)
            );

            if (status != STATUS_OK) {
                break;
            }

            server_handle_request(
                ctx,
                &session,
                &request,
                &response
            );

            status = named_pipe_write_exact(
                pipe_handle,
                &response,
                sizeof(response)
            );

            if (status != STATUS_OK) {
                break;
            }
        }

        auth_logout(&session);
        named_pipe_server_disconnect(pipe_handle);
    }

    return 0;
}

int server_run(server_ctx_t *ctx)
{
    if (ctx == NULL || ctx->bank == NULL) {
        return 1;
    }

    return (int)worker_thread_func(ctx);
}
