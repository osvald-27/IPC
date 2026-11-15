#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * server.exe [--workers N] [--fresh] [--nolock]
 *
 * The parent process owns the bank. It creates the shared memory, the
 * semaphore and the shutdown event, then starts N worker processes
 * with CreateProcess(). Each worker opens those objects by name and
 * serves clients on the named pipe.
 *
 * Ctrl+C (or "quit" at the console) sets the shutdown event. Workers
 * leave their loops, the parent waits for them, saves the bank and
 * closes every handle.
 */

static HANDLE g_stop = NULL;

/* Runs on a separate thread that Windows creates for the console event. */
static BOOL WINAPI on_console_event(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_stop != NULL) {
            SetEvent(g_stop);
        }
        return TRUE;        /* handled, do not kill the process */
    default:
        return FALSE;
    }
}

static int run_worker(int worker_id, int use_lock) {
    server_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.worker_id = worker_id;
    ctx.use_lock = use_lock;

    ctx.stop = OpenEventA(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, SHUTDOWN_EVENT_NAME);
    if (ctx.stop == NULL) {
        fprintf(stderr, "worker %d: OpenEvent failed: %lu\n", worker_id, GetLastError());
        return 1;
    }
    g_stop = ctx.stop;
    if (!SetConsoleCtrlHandler(on_console_event, TRUE)) {
        fprintf(stderr, "worker %d: SetConsoleCtrlHandler failed: %lu\n", worker_id, GetLastError());
    }

    int code = 1;
    if (shared_memory_open(&ctx.shm, 0) == STATUS_OK) {
        ctx.bank = shared_memory_bank(ctx.shm);
        if (semaphore_open(SEM_NAME, 0, &ctx.lock) == STATUS_OK) {
            code = worker_run(&ctx);
            semaphore_close(ctx.lock);
        }
        shared_memory_close(ctx.shm);
    }
    CloseHandle(ctx.stop);
    return code;
}

static int start_worker(int worker_id, int use_lock, PROCESS_INFORMATION *pi) {
    char exe[MAX_PATH];
    char command[MAX_PATH + 64];
    STARTUPINFOA si;

    if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) {
        fprintf(stderr, "GetModuleFileName failed: %lu\n", GetLastError());
        return 0;
    }
    snprintf(command, sizeof command, "\"%s\" --worker %d%s", exe, worker_id, use_lock ? "" : " --nolock");

    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(pi, sizeof *pi);

    /* no handles are inherited: the worker finds everything by name */
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi)) {
        fprintf(stderr, "CreateProcess for worker %d failed: %lu\n", worker_id, GetLastError());
        return 0;
    }
    CloseHandle(pi->hThread);
    return 1;
}

static int run_server(int workers, int fresh, int use_lock) {
    server_ctx_t ctx;
    PROCESS_INFORMATION procs[MAX_WORKERS];
    HANDLE waits[MAX_WORKERS + 1];
    int started = 0;
    int code = 1;

    memset(&ctx, 0, sizeof ctx);
    ctx.use_lock = use_lock;

    /* manual-reset so one SetEvent wakes every process waiting on it */
    ctx.stop = CreateEventA(NULL, TRUE, FALSE, SHUTDOWN_EVENT_NAME);
    if (ctx.stop == NULL) {
        fprintf(stderr, "CreateEvent failed: %lu\n", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "a server is already running\n");
        CloseHandle(ctx.stop);
        return 1;
    }
    g_stop = ctx.stop;
    if (!SetConsoleCtrlHandler(on_console_event, TRUE)) {
        fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
    }

    if (shared_memory_create(&ctx.shm) != STATUS_OK) {
        fprintf(stderr, "could not create the shared memory\n");
        goto close_event;
    }
    ctx.bank = shared_memory_bank(ctx.shm);

    registry_init(ctx.bank);
    if (!fresh && persistence_load(ctx.bank) == STATUS_OK) {
        printf("balances loaded from %s\n", DATA_FILE);
    }

    if (semaphore_create(SEM_NAME, 0, &ctx.lock) != STATUS_OK) {
        fprintf(stderr, "could not create the semaphore\n");
        goto close_memory;
    }

    for (int i = 0; i < workers; i++) {
        if (!start_worker(i + 1, use_lock, &procs[started])) {
            break;
        }
        started++;
    }
    if (started == 0) {
        fprintf(stderr, "no worker could be started\n");
        goto close_lock;
    }

    printf("server ready: %d worker processes, pipe %s%s\n", started, PIPE_NAME,
           use_lock ? "" : "  (NO LOCK: lost-update demonstration)");
    printf("type status or quit, or press Ctrl+C\n");
    fflush(stdout);
    operator_print_status(&ctx);
    operator_start(&ctx);

    /* sleep until shutdown is asked for, or a worker dies on its own */
    waits[0] = ctx.stop;
    for (int i = 0; i < started; i++) {
        waits[i + 1] = procs[i].hProcess;
    }
    DWORD r = WaitForMultipleObjects((DWORD)started + 1, waits, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0) {
        fprintf(stderr, "a worker stopped unexpectedly, shutting down\n");
    }

    printf("shutting down...\n");
    fflush(stdout);
    SetEvent(ctx.stop);

    /* give the workers five seconds to finish and close their handles */
    if (WaitForMultipleObjects((DWORD)started, &waits[1], TRUE, 5000) == WAIT_TIMEOUT) {
        fprintf(stderr, "some workers did not stop in time, terminating them\n");
        for (int i = 0; i < started; i++) {
            TerminateProcess(procs[i].hProcess, 1);
        }
    }
    for (int i = 0; i < started; i++) {
        CloseHandle(procs[i].hProcess);
    }

    operator_print_status(&ctx);
    if (persistence_save(ctx.bank) == STATUS_OK) {
        printf("balances saved to %s\n", DATA_FILE);
    }
    code = 0;

close_lock:
    semaphore_remove(ctx.lock);
close_memory:
    shared_memory_close(ctx.shm);
close_event:
    CloseHandle(ctx.stop);
    printf("server stopped, all handles closed\n");
    return code;
}

int main(int argc, char **argv) {
    int workers = DEFAULT_WORKERS;
    int worker_id = 0;
    int fresh = 0;
    int use_lock = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--worker") == 0 && i + 1 < argc) {
            worker_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fresh") == 0) {
            fresh = 1;
        } else if (strcmp(argv[i], "--nolock") == 0) {
            use_lock = 0;
        } else {
            fprintf(stderr, "usage: server [--workers N] [--fresh] [--nolock]\n");
            return 2;
        }
    }

    if (worker_id > 0) {
        return run_worker(worker_id, use_lock);
    }
    if (workers < 1 || workers > MAX_WORKERS) {
        fprintf(stderr, "--workers must be between 1 and %d\n", MAX_WORKERS);
        return 2;
    }
    return run_server(workers, fresh, use_lock);
}
