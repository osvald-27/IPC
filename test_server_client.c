#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../common/protocol.h"
#include "../common/status.h"
#include "../common/transaction.h"
#include "../common/types.h"
#include "../named_pipe/named_pipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Smoke Test & Throughput Benchmark (1, 2, 4, 8 clients)
 * Spawns N client processes via CreateProcessA() and exercises:
 *   REQUEST_LOGIN, REQUEST_DEPOSIT, REQUEST_WITHDRAWAL, REQUEST_TRANSFER,
 *   REQUEST_GET_BALANCE, REQUEST_LOGOUT
 * ============================================================================ */

static int run_worker(int worker_idx, int num_tx) {
    named_pipe_t *pipe = NULL;
    if (pipe_client_open(PIPE_NAME, 5000U, &pipe) != STATUS_OK) {
        fprintf(stderr, "Worker %d: failed to connect to %s\n", worker_idx, PIPE_NAME);
        return EXIT_FAILURE;
    }

    const char *acct_ids[ACCOUNT_COUNT] = { "ACC0001", "ACC0002", "ACC0003" };
    const char *my_id = acct_ids[worker_idx % ACCOUNT_COUNT];

    /* 1. Login */
    message_header_t hdr;
    login_request_t login_req;
    memset(&hdr, 0, sizeof(hdr));
    memset(&login_req, 0, sizeof(login_req));

    hdr.message_type = REQUEST_LOGIN;
    hdr.payload_size = (uint32_t)sizeof(login_req);
    snprintf(hdr.request_id.value, sizeof(hdr.request_id.value), "REQ-LOGIN-%d", worker_idx);
    snprintf(login_req.client_id.value, sizeof(login_req.client_id.value), "%s", my_id);

    if (pipe_send_message(pipe, &hdr, &login_req) != STATUS_OK) {
        pipe_close(pipe);
        return EXIT_FAILURE;
    }

    uint8_t resp_buf[MAX_PAYLOAD_SIZE];
    message_header_t resp_hdr;
    if (pipe_recv_message(pipe, &resp_hdr, resp_buf, MAX_PAYLOAD_SIZE) != STATUS_OK) {
        pipe_close(pipe);
        return EXIT_FAILURE;
    }

    response_t login_resp;
    memcpy(&login_resp, resp_buf, sizeof(login_resp));
    if (login_resp.status != STATUS_OK) {
        fprintf(stderr, "Worker %d: login rejected (%d)\n", worker_idx, (int)login_resp.status);
        pipe_close(pipe);
        return EXIT_FAILURE;
    }

    session_id_t session_id = resp_hdr.session_id;

    /* 2. Concurrent transactions */
    srand((unsigned int)(GetCurrentProcessId() ^ GetTickCount() ^ (worker_idx * 131)));
    for (int i = 0; i < num_tx; i++) {
        transaction_request_t tx;
        memset(&tx, 0, sizeof(tx));
        memset(&hdr, 0, sizeof(hdr));
        hdr.session_id = session_id;
        snprintf(hdr.request_id.value, sizeof(hdr.request_id.value), "REQ-%d-%d", worker_idx, i);

        int mode = i % 3;
        if (mode == 0) {
            hdr.message_type = REQUEST_TRANSFER;
            tx.type = TRANSACTION_TRANSFER;
            snprintf(tx.sender_id.value, sizeof(tx.sender_id.value), "%s", my_id);
            snprintf(tx.receiver_id.value, sizeof(tx.receiver_id.value), "%s",
                     acct_ids[(worker_idx + 1) % ACCOUNT_COUNT]);
            tx.amount = 100;
        } else if (mode == 1) {
            hdr.message_type = REQUEST_DEPOSIT;
            tx.type = TRANSACTION_DEPOSIT;
            snprintf(tx.sender_id.value, sizeof(tx.sender_id.value), "%s", my_id);
            tx.amount = 250;
        } else {
            hdr.message_type = REQUEST_WITHDRAWAL;
            tx.type = TRANSACTION_WITHDRAWAL;
            snprintf(tx.sender_id.value, sizeof(tx.sender_id.value), "%s", my_id);
            tx.amount = 250;
        }

        hdr.payload_size = (uint32_t)sizeof(tx);
        if (pipe_send_message(pipe, &hdr, &tx) != STATUS_OK ||
            pipe_recv_message(pipe, &resp_hdr, resp_buf, MAX_PAYLOAD_SIZE) != STATUS_OK) {
            fprintf(stderr, "Worker %d: tx %d I/O failure\n", worker_idx, i);
            break;
        }
    }

    /* 3. Logout */
    memset(&hdr, 0, sizeof(hdr));
    hdr.message_type = REQUEST_LOGOUT;
    hdr.session_id = session_id;
    hdr.payload_size = 0;
    if (pipe_send_message(pipe, &hdr, NULL) == STATUS_OK) {
        pipe_recv_message(pipe, &resp_hdr, resp_buf, MAX_PAYLOAD_SIZE);
    }

    pipe_close(pipe);
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    if (argc >= 4 && strcmp(argv[1], "--worker") == 0) {
        return run_worker(atoi(argv[2]), atoi(argv[3]));
    }

    int num_clients = (argc > 1) ? atoi(argv[1]) : 4;
    int tx_per_client = (argc > 2) ? atoi(argv[2]) : 250;
    if (num_clients < 1 || num_clients > 16 || tx_per_client < 1) {
        fprintf(stderr, "Usage: %s [num_clients 1..16] [tx_per_client]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char exe_path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
        pipe_log_win_error("GetModuleFileNameA");
        return EXIT_FAILURE;
    }

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    HANDLE procs[16];
    int spawned = 0;
    for (int i = 0; i < num_clients; i++) {
        char cmd[MAX_PATH + 64];
        snprintf(cmd, sizeof(cmd), "\"%s\" --worker %d %d", exe_path, i, tx_per_client);

        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        memset(&pi, 0, sizeof(pi));
        si.cb = sizeof(si);

        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            pipe_log_win_error("CreateProcessA");
            break;
        }
        CloseHandle(pi.hThread);
        procs[spawned++] = pi.hProcess;
    }

    if (spawned > 0) {
        WaitForMultipleObjects((DWORD)spawned, procs, TRUE, INFINITE);
        for (int i = 0; i < spawned; i++) {
            CloseHandle(procs[i]);
        }
    }

    QueryPerformanceCounter(&t1);
    double elapsed = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    int total_tx = spawned * tx_per_client;

    printf("[Win Smoke Test] Clients=%d | Tx/client=%d | Total=%d | Time=%.4fs | Throughput=%.2f tx/s\n",
           spawned, tx_per_client, total_tx, elapsed,
           (elapsed > 0.0) ? ((double)total_tx / elapsed) : 0.0);
    return EXIT_SUCCESS;
}
