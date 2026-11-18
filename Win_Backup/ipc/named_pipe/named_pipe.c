#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "named_pipe.h"

static int read_all(HANDLE h, void *buffer, DWORD length) {
    BYTE *p = (BYTE *)buffer;
    DWORD done = 0;
    while (done < length) {
        DWORD n = 0;
        if (!ReadFile(h, p + done, length - done, &n, NULL) || n == 0) return -1;
        done += n;
    }
    return 0;
}
static int write_all(HANDLE h, const void *buffer, DWORD length) {
    const BYTE *p = (const BYTE *)buffer;
    DWORD done = 0;
    while (done < length) {
        DWORD n = 0;
        if (!WriteFile(h, p + done, length - done, &n, NULL) || n == 0) return -1;
        done += n;
    }
    return 0;
}
int named_pipe_connect(const char *name, named_pipe_handle_t *out) {
    if (!name || !out) return -1;
    *out = NULL;
    for (int attempt = 0; attempt < 30; ++attempt) {
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            *out = h;
            return 0;
        }
        DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) return -1;
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeA(name, 1000);
        else Sleep(250);
    }
    return -1;
}
int named_pipe_exchange(named_pipe_handle_t pipe, const Message *request, Message *response) {
    if (!pipe || !request || !response) return -1;
    if (write_all((HANDLE)pipe, request, (DWORD)sizeof(*request)) != 0) return -1;
    return read_all((HANDLE)pipe, response, (DWORD)sizeof(*response));
}
void named_pipe_close(named_pipe_handle_t pipe) {
    if (pipe) CloseHandle((HANDLE)pipe);
}
