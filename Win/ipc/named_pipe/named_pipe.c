#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#include "named_pipe.h"

status_t named_pipe_server_create(HANDLE *pipe_handle)
{
    HANDLE handle;

    if (pipe_handle == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    *pipe_handle = INVALID_HANDLE_VALUE;

    handle = CreateNamedPipeA(
        CROSSIPC_PIPE_NAME,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        CROSSIPC_PIPE_BUFFER,
        CROSSIPC_PIPE_BUFFER,
        5000,
        NULL
    );

    if (handle == INVALID_HANDLE_VALUE) {
        return STATUS_QUEUE_ERROR;
    }

    *pipe_handle = handle;
    return STATUS_OK;
}

status_t named_pipe_server_accept(HANDLE pipe_handle)
{
    BOOL connected;

    if (pipe_handle == INVALID_HANDLE_VALUE || pipe_handle == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    connected = ConnectNamedPipe(pipe_handle, NULL);

    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        return STATUS_QUEUE_ERROR;
    }

    return STATUS_OK;
}

status_t named_pipe_client_connect(HANDLE *pipe_handle)
{
    HANDLE handle;

    if (pipe_handle == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    *pipe_handle = INVALID_HANDLE_VALUE;

    if (!WaitNamedPipeA(CROSSIPC_PIPE_NAME, 5000)) {
        return STATUS_SERVER_UNAVAILABLE;
    }

    handle = CreateFileA(
        CROSSIPC_PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (handle == INVALID_HANDLE_VALUE) {
        return STATUS_SERVER_UNAVAILABLE;
    }

    *pipe_handle = handle;
    return STATUS_OK;
}

status_t named_pipe_read_exact(HANDLE pipe_handle, void *buffer, size_t length)
{
    unsigned char *cursor = (unsigned char *)buffer;
    size_t received = 0;

    if (pipe_handle == INVALID_HANDLE_VALUE || buffer == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    while (received < length) {
        DWORD chunk = 0;
        DWORD requested = (DWORD)((length - received) > 0x7fffffffU ? 0x7fffffffU : (length - received));

        if (!ReadFile(pipe_handle, cursor + received, requested, &chunk, NULL)) {
            return STATUS_QUEUE_ERROR;
        }

        if (chunk == 0) {
            return STATUS_SERVER_UNAVAILABLE;
        }

        received += chunk;
    }

    return STATUS_OK;
}

status_t named_pipe_write_exact(HANDLE pipe_handle, const void *buffer, size_t length)
{
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t sent = 0;

    if (pipe_handle == INVALID_HANDLE_VALUE || buffer == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    while (sent < length) {
        DWORD chunk = 0;
        DWORD requested = (DWORD)((length - sent) > 0x7fffffffU ? 0x7fffffffU : (length - sent));

        if (!WriteFile(pipe_handle, cursor + sent, requested, &chunk, NULL)) {
            return STATUS_QUEUE_ERROR;
        }

        if (chunk == 0) {
            return STATUS_SERVER_UNAVAILABLE;
        }

        sent += chunk;
    }

    return STATUS_OK;
}

void named_pipe_disconnect(HANDLE pipe_handle)
{
    if (pipe_handle != INVALID_HANDLE_VALUE && pipe_handle != NULL) {
        FlushFileBuffers(pipe_handle);
        DisconnectNamedPipe(pipe_handle);
        CloseHandle(pipe_handle);
    }
}

void named_pipe_server_disconnect(HANDLE pipe_handle)
{
    if (pipe_handle != INVALID_HANDLE_VALUE && pipe_handle != NULL) {
        DisconnectNamedPipe(pipe_handle);
        CloseHandle(pipe_handle);
    }
}
