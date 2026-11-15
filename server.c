#include "server_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server - Core Lifecycle & Multi-Client Accept Loop
 * Owner: @William (Win/ipc/server)
 *
 * Manages:
 *   - CreateFileMappingA() / MapViewOfFile() for shared bank_t state
 *   - CreateSemaphoreA() for named semaphore synchronization
 *   - SetConsoleCtrlHandler() for clean Ctrl+C shutdown & handle cleanup
 *   - Multi-instance Win32 Named Pipe accept loop with CreateThread() workers
 * ============================================================================ */

#define MAX_WORKER_THREADS 64

typedef struct {
    server_context_t *ctx;
    named_pipe_t     *pipe;
} worker_arg_t;

static server_context_t *g_active_ctx = NULL;

static void wake_accept_loop(void) {
    /* Brief dummy client connection wakes ConnectNamedPipe() on Ctrl+C */
    HANDLE hWake = CreateFileA(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );
    if (hWake != INVALID_HANDLE_VALUE) {
        CloseHandle(hWake);
    }
}

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            if (g_active_ctx) {
                InterlockedExchange(&g_active_ctx->stop_requested, 1);
                wake_accept_loop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

static void send_error_reply(named_pipe_t *pipe,
                             const message_header_t *req_hdr,
                             status_t err_status) {
    message_header_t resp_hdr;
    response_t resp;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    resp_hdr.message_type = RESPONSE_MESSAGE;
    resp_hdr.payload_size = (uint32_t)sizeof(response_t);
    resp_hdr.request_id = req_hdr->request_id;
    resp_hdr.session_id = req_hdr->session_id;

    memset(&resp, 0, sizeof(resp));
    resp.status = err_status;
    resp.payload_size = 0;

    pipe_send_message(pipe, &resp_hdr, &resp);
}

static void dispatch_request(server_context_t *ctx,
                             named_pipe_t *pipe,
                             const message_header_t *req_hdr,
                             const uint8_t *payload) {
    message_header_t resp_hdr;
    memset(&resp_hdr, 0, sizeof(resp_hdr));
    resp_hdr.message_type = RESPONSE_MESSAGE;
    resp_hdr.request_id = req_hdr->request_id;
    resp_hdr.session_id = req_hdr->session_id;

    switch ((message_type_t)req_hdr->message_type) {
        case REQUEST_INITIALIZE:
        case REQUEST_LOGIN: {
            if (req_hdr->payload_size < sizeof(login_request_t)) {
                send_error_reply(pipe, req_hdr, STATUS_INVALID_ARGUMENT);
                return;
            }
            login_request_t login_req;
            memcpy(&login_req, payload, sizeof(login_req));

            session_id_t issued_session;
            status_t st = auth_handle_login(ctx, &login_req, &issued_session);

            uint8_t out_buf[sizeof(response_t) + sizeof(session_id_t)];
            response_t resp;
            resp.status = st;
            resp.payload_size = (st == STATUS_OK) ? (uint32_t)sizeof(session_id_t) : 0U;
            memcpy(out_buf, &resp, sizeof(resp));
            if (st == STATUS_OK) {
                resp_hdr.session_id = issued_session;
                memcpy(out_buf + sizeof(resp), &issued_session, sizeof(issued_session));
            }
            resp_hdr.payload_size = (uint32_t)(sizeof(resp) + resp.payload_size);
            pipe_send_message(pipe, &resp_hdr, out_buf);
            break;
        }
        case REQUEST_LOGOUT: {
            status_t st = auth_handle_logout(&req_hdr->session_id);
            response_t resp;
            resp.status = st;
            resp.payload_size = 0;
            resp_hdr.payload_size = (uint32_t)sizeof(resp);
            pipe_send_message(pipe, &resp_hdr, &resp);
            break;
        }
        case REQUEST_GET_BALANCE: {
            if (req_hdr->payload_size < sizeof(balance_request_t)) {
                send_error_reply(pipe, req_hdr, STATUS_INVALID_ARGUMENT);
                return;
            }
            balance_request_t bal_req;
            memcpy(&bal_req, payload, sizeof(bal_req));

            /* Validate session when client provided one */
            if (req_hdr->session_id.value[0] != '\0') {
                client_id_t authed_id;
                status_t auth_st = auth_require_session(&req_hdr->session_id, &authed_id);
                if (auth_st != STATUS_OK) {
                    send_error_reply(pipe, req_hdr, auth_st);
                    return;
                }
                if (bal_req.client_id.value[0] == '\0') {
                    bal_req.client_id = authed_id;
                }
            }

            balance_response_t bal_resp;
            status_t st = tx_handle_balance(ctx, &bal_req, &bal_resp);

            uint8_t out_buf[sizeof(response_t) + sizeof(balance_response_t)];
            response_t resp;
            resp.status = st;
            resp.payload_size = (st == STATUS_OK) ? (uint32_t)sizeof(balance_response_t) : 0U;
            memcpy(out_buf, &resp, sizeof(resp));
            if (st == STATUS_OK) {
                memcpy(out_buf + sizeof(resp), &bal_resp, sizeof(bal_resp));
            }
            resp_hdr.payload_size = (uint32_t)(sizeof(resp) + resp.payload_size);
            pipe_send_message(pipe, &resp_hdr, out_buf);
            break;
        }
        case REQUEST_DEPOSIT:
        case REQUEST_WITHDRAWAL:
        case REQUEST_TRANSFER: {
            if (req_hdr->payload_size < sizeof(transaction_request_t)) {
                send_error_reply(pipe, req_hdr, STATUS_INVALID_ARGUMENT);
                return;
            }
            transaction_request_t tx_req;
            memcpy(&tx_req, payload, sizeof(tx_req));

            if (req_hdr->message_type == REQUEST_DEPOSIT) {
                tx_req.type = TRANSACTION_DEPOSIT;
            } else if (req_hdr->message_type == REQUEST_WITHDRAWAL) {
                tx_req.type = TRANSACTION_WITHDRAWAL;
            } else {
                tx_req.type = TRANSACTION_TRANSFER;
            }

            if (req_hdr->session_id.value[0] != '\0') {
                client_id_t authed_id;
                status_t auth_st = auth_require_session(&req_hdr->session_id, &authed_id);
                if (auth_st != STATUS_OK) {
                    send_error_reply(pipe, req_hdr, auth_st);
                    return;
                }
                if (tx_req.sender_id.value[0] == '\0') {
                    tx_req.sender_id = authed_id;
                }
            }

            transaction_response_t tx_resp;
            status_t st = tx_handle_operation(ctx, &tx_req, &tx_resp);

            uint8_t out_buf[sizeof(response_t) + sizeof(transaction_response_t)];
            response_t resp;
            resp.status = st;
            resp.payload_size = (uint32_t)sizeof(transaction_response_t);
            memcpy(out_buf, &resp, sizeof(resp));
            memcpy(out_buf + sizeof(resp), &tx_resp, sizeof(tx_resp));
            resp_hdr.payload_size = (uint32_t)sizeof(out_buf);
            pipe_send_message(pipe, &resp_hdr, out_buf);
            break;
        }
        case REQUEST_SERVER_STATUS: {
            uint8_t out_buf[sizeof(response_t) + 256];
            response_t resp;
            char summary[256];
            status_t st = operator_handle_status(ctx, &resp, summary, (uint32_t)sizeof(summary));
            if (st != STATUS_OK) {
                send_error_reply(pipe, req_hdr, st);
                return;
            }
            memcpy(out_buf, &resp, sizeof(resp));
            memcpy(out_buf + sizeof(resp), summary, resp.payload_size);
            resp_hdr.payload_size = (uint32_t)(sizeof(resp) + resp.payload_size);
            pipe_send_message(pipe, &resp_hdr, out_buf);
            break;
        }
        case REQUEST_TRANSACTION_STATUS: {
            response_t resp;
            resp.status = STATUS_OK;
            resp.payload_size = 0;
            resp_hdr.payload_size = (uint32_t)sizeof(resp);
            pipe_send_message(pipe, &resp_hdr, &resp);
            break;
        }
        default: {
            send_error_reply(pipe, req_hdr, STATUS_INVALID_ARGUMENT);
            break;
        }
    }
}

static DWORD WINAPI client_worker_thread(LPVOID param) {
    worker_arg_t *arg = (worker_arg_t *)param;
    server_context_t *ctx = arg->ctx;
    named_pipe_t *pipe = arg->pipe;
    free(arg);

    uint8_t payload[MAX_PAYLOAD_SIZE];
    message_header_t hdr;

    while (!InterlockedCompareExchange(&ctx->stop_requested, 0, 0)) {
        status_t st = pipe_recv_message(pipe, &hdr, payload, MAX_PAYLOAD_SIZE);
        if (st == STATUS_QUEUE_EMPTY) {
            break; /* Client closed its handle cleanly */
        }
        if (st != STATUS_OK) {
            break;
        }
        dispatch_request(ctx, pipe, &hdr, payload);
    }

    pipe_server_disconnect(pipe);
    pipe_close(pipe);
    return 0;
}

status_t server_init(server_context_t *ctx, int sync_enabled) {
    if (!ctx) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->sync_enabled = sync_enabled;
    InitializeCriticalSection(&ctx->audit_cs);
    QueryPerformanceFrequency(&ctx->qpc_freq);

    registry_init();
    persistence_init(ctx);

    ctx->shm_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        (DWORD)sizeof(bank_t),
        SHM_NAME
    );
    if (!ctx->shm_handle) {
        pipe_log_win_error("CreateFileMappingA");
        return STATUS_SHARED_MEMORY_ERROR;
    }

    ctx->bank = (bank_t *)MapViewOfFile(
        ctx->shm_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(bank_t)
    );
    if (!ctx->bank) {
        pipe_log_win_error("MapViewOfFile");
        CloseHandle(ctx->shm_handle);
        ctx->shm_handle = NULL;
        return STATUS_SHARED_MEMORY_ERROR;
    }

    memset(ctx->bank, 0, sizeof(bank_t));
    for (uint32_t i = 0; i < ACCOUNT_COUNT; i++) {
        snprintf(ctx->bank->accounts[i].client_id.value,
                 sizeof(ctx->bank->accounts[i].client_id.value),
                 "ACC%04u",
                 (unsigned int)(i + 1U));
        ctx->bank->accounts[i].balance = INITIAL_BALANCE;
        ctx->bank->accounts[i].version = 1ULL;
        ctx->initial_total += INITIAL_BALANCE;
    }

    ctx->sem_handle = CreateSemaphoreA(NULL, 1, 1, SEM_NAME);
    if (!ctx->sem_handle) {
        pipe_log_win_error("CreateSemaphoreA");
        UnmapViewOfFile(ctx->bank);
        ctx->bank = NULL;
        CloseHandle(ctx->shm_handle);
        ctx->shm_handle = NULL;
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    g_active_ctx = ctx;
    if (!SetConsoleCtrlHandler(console_ctrl_handler, TRUE)) {
        pipe_log_win_error("SetConsoleCtrlHandler");
        return STATUS_INTERNAL_ERROR;
    }

    operator_print_banner(ctx);
    return STATUS_OK;
}

status_t server_run(server_context_t *ctx) {
    if (!ctx) {
        return STATUS_INVALID_ARGUMENT;
    }

    HANDLE worker_threads[MAX_WORKER_THREADS];
    DWORD worker_count = 0;

    while (!InterlockedCompareExchange(&ctx->stop_requested, 0, 0)) {
        named_pipe_t *pipe = NULL;
        if (pipe_server_create(PIPE_NAME, &pipe) != STATUS_OK) {
            break;
        }

        if (pipe_server_accept(pipe) != STATUS_OK) {
            pipe_close(pipe);
            if (InterlockedCompareExchange(&ctx->stop_requested, 0, 0)) {
                break;
            }
            continue;
        }

        if (InterlockedCompareExchange(&ctx->stop_requested, 0, 0)) {
            pipe_server_disconnect(pipe);
            pipe_close(pipe);
            break;
        }

        /* Reap completed worker thread handles before spawning new ones */
        for (DWORD i = 0; i < worker_count; ) {
            if (WaitForSingleObject(worker_threads[i], 0) == WAIT_OBJECT_0) {
                CloseHandle(worker_threads[i]);
                worker_threads[i] = worker_threads[--worker_count];
            } else {
                i++;
            }
        }

        if (worker_count >= MAX_WORKER_THREADS) {
            fprintf(stderr, "[server] Max worker threads (%d) reached; rejecting client.\n",
                    MAX_WORKER_THREADS);
            pipe_server_disconnect(pipe);
            pipe_close(pipe);
            continue;
        }

        worker_arg_t *arg = (worker_arg_t *)malloc(sizeof(worker_arg_t));
        if (!arg) {
            pipe_server_disconnect(pipe);
            pipe_close(pipe);
            continue;
        }
        arg->ctx = ctx;
        arg->pipe = pipe;

        HANDLE hThread = CreateThread(NULL, 0, client_worker_thread, arg, 0, NULL);
        if (!hThread) {
            pipe_log_win_error("CreateThread");
            free(arg);
            pipe_server_disconnect(pipe);
            pipe_close(pipe);
            continue;
        }
        worker_threads[worker_count++] = hThread;
    }

    if (worker_count > 0) {
        WaitForMultipleObjects(worker_count, worker_threads, TRUE, 5000);
        for (DWORD i = 0; i < worker_count; i++) {
            CloseHandle(worker_threads[i]);
        }
    }

    return STATUS_OK;
}

void server_shutdown(server_context_t *ctx) {
    if (!ctx) {
        return;
    }

    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    g_active_ctx = NULL;

    persistence_print_report(ctx);
    persistence_close();

    if (ctx->bank) {
        if (!UnmapViewOfFile(ctx->bank)) {
            pipe_log_win_error("UnmapViewOfFile");
        }
        ctx->bank = NULL;
    }
    if (ctx->shm_handle) {
        if (!CloseHandle(ctx->shm_handle)) {
            pipe_log_win_error("CloseHandle(shm)");
        }
        ctx->shm_handle = NULL;
    }
    if (ctx->sem_handle) {
        if (!CloseHandle(ctx->sem_handle)) {
            pipe_log_win_error("CloseHandle(sem)");
        }
        ctx->sem_handle = NULL;
    }

    registry_destroy();
    DeleteCriticalSection(&ctx->audit_cs);
    printf("[CrossIPC Windows Server] All Win32 handles closed cleanly.\n");
}
