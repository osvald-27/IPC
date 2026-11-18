#include "server.h"

#include <direct.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG g_shutdown = 0;
static HANDLE g_workers[MAX_WORKERS];
static int g_num_workers = 0;

static BOOL WINAPI handle_console_ctrl(DWORD ctrl_type) {
    switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        InterlockedExchange(&g_shutdown, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

static int server_shm_init(const char *shm_name, HANDLE *shm_handle, bank_t **bank) {
    HANDLE h = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        (DWORD)sizeof(bank_t),
        shm_name
    );
    if (h == NULL) {
        fprintf(stderr, "CreateFileMappingA failed (error %lu)\n", GetLastError());
        return -1;
    }

    void *addr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bank_t));
    if (addr == NULL) {
        fprintf(stderr, "MapViewOfFile failed (error %lu)\n", GetLastError());
        CloseHandle(h);
        return -1;
    }

    *shm_handle = h;
    *bank = (bank_t *)addr;
    return 0;
}

/* Wakes up worker threads blocked inside ConnectNamedPipe during shutdown */
static void wake_pipe_workers(int count) {
    for (int i = 0; i < count; i++) {
        HANDLE dummy = CreateFileA(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (dummy != INVALID_HANDLE_VALUE) {
            CloseHandle(dummy);
        }
    }
}

static DWORD WINAPI worker_entry(LPVOID arg) {
    server_ctx_t *worker_ctx = (server_ctx_t *)arg;
    int code = server_run(worker_ctx);
    free(worker_ctx);
    return (DWORD)code;
}

int main(int argc, char **argv) {
    int workers = DEFAULT_WORKERS;
    int fresh = 0;
    int use_lock = 1;
    int auto_approve = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fresh") == 0) {
            fresh = 1;
        } else if (strcmp(argv[i], "--nolock") == 0) {
            use_lock = 0;
        } else if (strcmp(argv[i], "--auto-approve") == 0) {
            auto_approve = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--workers N] [--fresh] [--nolock] [--auto-approve]\n", argv[0]);
            printf("  --workers N     Number of worker threads to spawn (default: %d, max: %d)\n",
                   DEFAULT_WORKERS, MAX_WORKERS);
            printf("  --fresh         Start with fresh accounts, ignoring saved bank.dat\n");
            printf("  --nolock        Disable semaphore lock to demonstrate lost-update flaw\n");
            printf("  --auto-approve  Automatically approve cash deposits (for tests/benchmarks)\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown argument '%s'. Use --help for usage.\n", argv[i]);
            return 2;
        }
    }

    if (workers < 1 || workers > MAX_WORKERS) {
        fprintf(stderr, "Error: --workers must be between 1 and %d\n", MAX_WORKERS);
        return 2;
    }

    /* Register console Ctrl+C / close handler for clean shutdown */
    SetConsoleCtrlHandler(handle_console_ctrl, TRUE);

    _mkdir("utilities");

    server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.use_lock = use_lock;
    ctx.auto_approve = auto_approve;
    ctx.worker_id = 0;
    ctx.pid = GetCurrentProcessId();

    printf("=====================================================\n");
    printf("   CrossIPC Windows Banking Server (CEF427 Group 10) \n");
    printf("=====================================================\n");

    /* 1. Create Windows Shared Memory (File Mapping) */
    if (server_shm_init(SHM_NAME, &ctx.shm_handle, &ctx.bank) != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        return 1;
    }

    /* 2. Initialize or recover account state */
    registry_init(ctx.bank);
    if (!fresh && persistence_load(ctx.bank) == STATUS_OK) {
        printf("[STORAGE] Account balances loaded from %s\n", DATA_FILE);
    } else {
        printf("[STORAGE] Initialized fresh accounts with %" PRId64 " %s each\n",
               INITIAL_BALANCE, CURRENCY_CODE);
    }

    /* 3. Create Windows Named Semaphore */
    status_t sem_st = semaphore_create(SEM_NAME, &ctx.lock);
    if (sem_st != STATUS_OK) {
        if (sem_st == STATUS_ALREADY_EXISTS) {
            semaphore_open(SEM_NAME, &ctx.lock);
        } else {
            fprintf(stderr, "Failed to create semaphore (%s)\n", server_status_string(sem_st));
            UnmapViewOfFile(ctx.bank);
            CloseHandle(ctx.shm_handle);
            return 1;
        }
    }

    printf("[IPC] Named Pipe:         %s\n", PIPE_NAME);
    printf("[IPC] Shared Memory:      %s\n", SHM_NAME);
    printf("[IPC] Concurrency Lock:   %s\n", use_lock ? "ENABLED (Windows Named Semaphore)" : "DISABLED (--nolock: Lost Update demonstration)");
    printf("[IPC] Deposit Approval:   %s\n", auto_approve ? "AUTO-APPROVE" : "OPERATOR REVIEW (approve/reject)");

    /* 4. Spawn Worker Threads */
    for (int i = 0; i < workers; i++) {
        server_ctx_t *worker_ctx = malloc(sizeof(server_ctx_t));
        if (worker_ctx == NULL) {
            fprintf(stderr, "Out of memory allocating worker %d\n", i + 1);
            break;
        }
        *worker_ctx = ctx;
        worker_ctx->worker_id = i + 1;

        HANDLE th = CreateThread(NULL, 0, worker_entry, worker_ctx, 0, NULL);
        if (th == NULL) {
            fprintf(stderr, "CreateThread worker %d failed (error %lu)\n", i + 1, GetLastError());
            free(worker_ctx);
            break;
        }
        g_workers[g_num_workers++] = th;
    }

    printf("[SERVER] Started %d worker threads successfully\n", g_num_workers);
    printf("[SERVER] Type 'status', 'pending', 'approve <TxnID>', 'quit', or press Ctrl+C\n");
    fflush(stdout);

    /* 5. Display initial status and start operator thread */
    operator_print_status(&ctx);
    operator_start(&ctx);

    /* 6. Server Main Loop: monitor shutdown flags */
    while (!g_shutdown && !ctx.stop) {
        Sleep(100);
    }

    /* 7. Graceful Shutdown Sequence */
    printf("\n[SERVER] Initiating graceful shutdown...\n");
    fflush(stdout);
    InterlockedExchange(&ctx.stop, 1);
    operator_stop();

    /* Unblock any workers waiting in ConnectNamedPipe() */
    wake_pipe_workers(g_num_workers);

    /* Wait for worker threads to exit cleanly */
    if (g_num_workers > 0) {
        WaitForMultipleObjects((DWORD)g_num_workers, g_workers, TRUE, 3000);
        for (int i = 0; i < g_num_workers; i++) {
            CloseHandle(g_workers[i]);
        }
    }

    /* Print final account consistency report */
    printf("\n[REPORT] Final Account State:\n");
    operator_print_status(&ctx);

    /* Save persistent state */
    if (persistence_save(ctx.bank) == STATUS_OK) {
        printf("[STORAGE] Account state saved to %s\n", DATA_FILE);
    }

    /* 8. Cleanup Windows IPC Resources */
    semaphore_remove(ctx.lock);
    UnmapViewOfFile(ctx.bank);
    CloseHandle(ctx.shm_handle);

    printf("[SERVER] Clean shutdown complete. All Windows IPC resources released.\n");
    return 0;
}
