#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * client.exe ACC0001                      interactive
 * client.exe --auto ACC0001 COUNT SEED    one automatic client
 * client.exe --spawn CLIENTS COUNT        start CLIENTS automatic clients with
 *                                         CreateProcess() and measure throughput
 */

#define MAX_SPAWN 64        /* WaitForMultipleObjects takes at most 64 handles */

static HANDLE g_stop = NULL;

static BOOL WINAPI on_console_event(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_stop != NULL) {
            SetEvent(g_stop);
        }
        return TRUE;
    }
    return FALSE;
}

static int spawn_clients(int clients, unsigned long count) {
    char exe[MAX_PATH];
    HANDLE procs[MAX_SPAWN];
    LARGE_INTEGER freq, t0, t1;
    int started = 0;

    if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) {
        fprintf(stderr, "GetModuleFileName failed: %lu\n", GetLastError());
        return 1;
    }

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    for (int i = 0; i < clients; i++) {
        char command[MAX_PATH + 80];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        /* clients are spread over the accounts, each with its own seed */
        snprintf(command, sizeof command, "\"%s\" --auto ACC%04u %lu %d", exe,
                 (unsigned)(i % (int)ACCOUNT_COUNT) + 1U, count, 1000 + i);

        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            fprintf(stderr, "CreateProcess for client %d failed: %lu\n", i + 1, GetLastError());
            break;
        }
        CloseHandle(pi.hThread);
        procs[started++] = pi.hProcess;
    }
    if (started == 0) {
        return 1;
    }

    /* wait for every child, the same job waitpid() does on Linux */
    if (WaitForMultipleObjects((DWORD)started, procs, TRUE, INFINITE) == WAIT_FAILED) {
        fprintf(stderr, "WaitForMultipleObjects failed: %lu\n", GetLastError());
    }
    QueryPerformanceCounter(&t1);

    unsigned long committed = 0;
    int failed = 0;
    for (int i = 0; i < started; i++) {
        DWORD code = 0;
        if (!GetExitCodeProcess(procs[i], &code) || code == (DWORD)-1) {
            failed++;
        } else {
            committed += code;      /* each client exits with its committed count */
        }
        CloseHandle(procs[i]);
    }

    double seconds = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    double tps = seconds > 0.0 ? (double)committed / seconds : 0.0;

    /* ask the server whether the totals still add up */
    client_session_t s;
    const char *check = "not checked";
    if (session_open(&s, "ACC0001", g_stop) == STATUS_OK) {
        check = session_server_check(&s) == STATUS_OK ? "consistent" : "NOT CONSISTENT";
        session_close(&s);
    }

    printf("clients=%d sent=%lu committed=%lu seconds=%.3f tps=%.0f totals=%s",
           started, count * (unsigned long)started, committed, seconds, tps, check);
    if (failed > 0) {
        printf(" failed_clients=%d", failed);
    }
    printf("\n");
    /* one line a spreadsheet can read: clients,committed,seconds,tps */
    fprintf(stderr, "CSV,%d,%lu,%.3f,%.0f\n", started, committed, seconds, tps);
    return failed > 0 ? 1 : 0;
}

static int usage(void) {
    fprintf(stderr, "usage: client ACCxxxx\n"
                    "       client --auto ACCxxxx COUNT SEED\n"
                    "       client --spawn CLIENTS COUNT\n");
    return 2;
}

int main(int argc, char **argv) {
    /* manual-reset, unnamed: only this process uses it */
    g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_stop == NULL) {
        fprintf(stderr, "CreateEvent failed: %lu\n", GetLastError());
        return 1;
    }
    if (!SetConsoleCtrlHandler(on_console_event, TRUE)) {
        fprintf(stderr, "SetConsoleCtrlHandler failed: %lu\n", GetLastError());
    }

    int code;
    if (argc == 2 && argv[1][0] != '-') {
        code = client_interactive(argv[1], g_stop);
    } else if (argc == 5 && strcmp(argv[1], "--auto") == 0) {
        code = client_auto(argv[2], strtoul(argv[3], NULL, 10), (unsigned)atoi(argv[4]), g_stop);
    } else if (argc == 4 && strcmp(argv[1], "--spawn") == 0) {
        int clients = atoi(argv[2]);
        if (clients < 1 || clients > MAX_SPAWN) {
            fprintf(stderr, "CLIENTS must be between 1 and %d\n", MAX_SPAWN);
            code = 2;
        } else {
            code = spawn_clients(clients, strtoul(argv[3], NULL, 10));
        }
    } else {
        code = usage();
    }

    CloseHandle(g_stop);
    return code;
}
