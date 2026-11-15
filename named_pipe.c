#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "named_pipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Named Pipe Implementation (Win32 API)
 * Owner: @William (Win/ipc/named_pipe)
 *
 * Uses:
 *   - CreateNamedPipeA() / ConnectNamedPipe() / DisconnectNamedPipe()
 *   - CreateFileA() / WaitNamedPipeA() / SetNamedPipeHandleState()
 *   - ReadFile() / WriteFile() / FlushFileBuffers() / CloseHandle()
 *   - GetLastError() / FormatMessageA() for strict Win32 error reporting
 * ============================================================================ */

struct named_pipe {
    HANDLE handle;
    int    is_server;
};

void pipe_log_win_error(const char *context) {
    DWORD err = GetLastError();
    char msg[256];
    memset(msg, 0, sizeof(msg));

    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        msg,
        (DWORD)sizeof(msg),
        NULL
    );

    size_t len = strlen(msg);
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) {
        msg[--len] = '\0';
    }

    fprintf(stderr, "[named_pipe] %s failed (GetLastError=%lu: %s)\n",
            context ? context : "Win32 API",
            (unsigned long)err,
            msg[0] != '\0' ? msg : "Unknown error");
}

status_t pipe_server_create(const char *pipe_name, named_pipe_t **out_pipe) {
    if (!pipe_name || !out_pipe) {
        return STATUS_INVALID_ARGUMENT;
    }

    *out_pipe = NULL;

    HANDLE hPipe = CreateNamedPipeA(
        pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        (DWORD)MAX_MESSAGE_SIZE,
        (DWORD)MAX_MESSAGE_SIZE,
        0,
        NULL
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        pipe_log_win_error("CreateNamedPipeA");
        return STATUS_QUEUE_ERROR;
    }

    named_pipe_t *p = (named_pipe_t *)calloc(1, sizeof(named_pipe_t));
    if (!p) {
        CloseHandle(hPipe);
        return STATUS_OUT_OF_MEMORY;
    }

    p->handle = hPipe;
    p->is_server = 1;
    *out_pipe = p;
    return STATUS_OK;
}

status_t pipe_server_accept(named_pipe_t *pipe) {
    if (!pipe || !pipe->is_server || pipe->handle == INVALID_HANDLE_VALUE) {
        return STATUS_INVALID_ARGUMENT;
    }

    BOOL ok = ConnectNamedPipe(pipe->handle, NULL);
    if (!ok) {
        DWORD err = GetLastError();
        /*
         * If a client connected in the tiny window between CreateNamedPipeA()
         * and ConnectNamedPipe(), Windows returns FALSE with ERROR_PIPE_CONNECTED.
         * That is a valid, active connection.
         */
        if (err != ERROR_PIPE_CONNECTED) {
            pipe_log_win_error("ConnectNamedPipe");
            return STATUS_QUEUE_ERROR;
        }
    }

    return STATUS_OK;
}

status_t pipe_server_disconnect(named_pipe_t *pipe) {
    if (!pipe || !pipe->is_server || pipe->handle == INVALID_HANDLE_VALUE) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (!FlushFileBuffers(pipe->handle)) {
        DWORD err = GetLastError();
        if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED) {
            pipe_log_win_error("FlushFileBuffers");
        }
    }

    if (!DisconnectNamedPipe(pipe->handle)) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_NOT_CONNECTED) {
            pipe_log_win_error("DisconnectNamedPipe");
            return STATUS_QUEUE_ERROR;
        }
    }

    return STATUS_OK;
}

status_t pipe_client_open(const char *pipe_name,
                          uint32_t timeout_ms,
                          named_pipe_t **out_pipe) {
    if (!pipe_name || !out_pipe) {
        return STATUS_INVALID_ARGUMENT;
    }

    *out_pipe = NULL;
    HANDLE hPipe = INVALID_HANDLE_VALUE;

    for (int attempt = 0; attempt < 20; attempt++) {
        hPipe = CreateFileA(
            pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }

        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeA(pipe_name, (DWORD)(timeout_ms > 0 ? timeout_ms : 5000U))) {
                pipe_log_win_error("WaitNamedPipeA");
                return STATUS_TIMEOUT;
            }
        } else if (err == ERROR_FILE_NOT_FOUND) {
            pipe_log_win_error("CreateFileA(pipe not found)");
            return STATUS_SERVER_UNAVAILABLE;
        } else {
            pipe_log_win_error("CreateFileA");
            return STATUS_QUEUE_ERROR;
        }
    }

    if (hPipe == INVALID_HANDLE_VALUE) {
        return STATUS_TIMEOUT;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &mode, NULL, NULL)) {
        pipe_log_win_error("SetNamedPipeHandleState");
        CloseHandle(hPipe);
        return STATUS_QUEUE_ERROR;
    }

    named_pipe_t *p = (named_pipe_t *)calloc(1, sizeof(named_pipe_t));
    if (!p) {
        CloseHandle(hPipe);
        return STATUS_OUT_OF_MEMORY;
    }

    p->handle = hPipe;
    p->is_server = 0;
    *out_pipe = p;
    return STATUS_OK;
}

status_t pipe_send_message(named_pipe_t *pipe,
                           const message_header_t *header,
                           const void *payload) {
    if (!pipe || pipe->handle == INVALID_HANDLE_VALUE || !header) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (header->payload_size > MAX_PAYLOAD_SIZE) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (header->payload_size > 0 && !payload) {
        return STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[sizeof(message_header_t) + MAX_PAYLOAD_SIZE];
    message_header_t wire_hdr = *header;
    wire_hdr.magic = PROTOCOL_NUMBER;
    wire_hdr.version = PROTOCOL_VERSION;
    wire_hdr.header_size = (uint16_t)sizeof(message_header_t);

    memcpy(frame, &wire_hdr, sizeof(message_header_t));
    if (wire_hdr.payload_size > 0) {
        memcpy(frame + sizeof(message_header_t), payload, wire_hdr.payload_size);
    }

    DWORD total_bytes = (DWORD)(sizeof(message_header_t) + wire_hdr.payload_size);
    DWORD bytes_written = 0;

    if (!WriteFile(pipe->handle, frame, total_bytes, &bytes_written, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
            return STATUS_QUEUE_EMPTY;
        }
        pipe_log_win_error("WriteFile(named pipe)");
        return STATUS_QUEUE_ERROR;
    }

    if (bytes_written != total_bytes) {
        fprintf(stderr, "[named_pipe] WriteFile truncated (%lu/%lu bytes)\n",
                (unsigned long)bytes_written, (unsigned long)total_bytes);
        return STATUS_QUEUE_ERROR;
    }

    return STATUS_OK;
}

status_t pipe_recv_message(named_pipe_t *pipe,
                           message_header_t *out_header,
                           void *out_payload,
                           uint32_t max_payload_size) {
    if (!pipe || pipe->handle == INVALID_HANDLE_VALUE || !out_header) {
        return STATUS_INVALID_ARGUMENT;
    }

    uint8_t frame[sizeof(message_header_t) + MAX_PAYLOAD_SIZE];
    DWORD bytes_read = 0;

    BOOL ok = ReadFile(
        pipe->handle,
        frame,
        (DWORD)sizeof(frame),
        &bytes_read,
        NULL
    );

    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
            return STATUS_QUEUE_EMPTY;
        }
        pipe_log_win_error("ReadFile(named pipe)");
        return STATUS_QUEUE_ERROR;
    }

    if (bytes_read == 0) {
        return STATUS_QUEUE_EMPTY;
    }

    if (bytes_read < (DWORD)sizeof(message_header_t)) {
        fprintf(stderr, "[named_pipe] Short header (%lu bytes)\n",
                (unsigned long)bytes_read);
        return STATUS_QUEUE_ERROR;
    }

    memcpy(out_header, frame, sizeof(message_header_t));

    if (out_header->magic != PROTOCOL_NUMBER ||
        out_header->version != PROTOCOL_VERSION ||
        out_header->header_size != (uint16_t)sizeof(message_header_t)) {
        fprintf(stderr, "[named_pipe] Protocol mismatch (magic=0x%08X ver=%u)\n",
                (unsigned int)out_header->magic, (unsigned int)out_header->version);
        return STATUS_QUEUE_ERROR;
    }

    if (out_header->payload_size > MAX_PAYLOAD_SIZE ||
        out_header->payload_size > max_payload_size) {
        fprintf(stderr, "[named_pipe] Payload size %u exceeds limit %u\n",
                (unsigned int)out_header->payload_size,
                (unsigned int)max_payload_size);
        return STATUS_INVALID_ARGUMENT;
    }

    if ((DWORD)(sizeof(message_header_t) + out_header->payload_size) != bytes_read) {
        fprintf(stderr, "[named_pipe] Frame length mismatch (got %lu, expected %lu)\n",
                (unsigned long)bytes_read,
                (unsigned long)(sizeof(message_header_t) + out_header->payload_size));
        return STATUS_QUEUE_ERROR;
    }

    if (out_header->payload_size > 0 && out_payload) {
        memcpy(out_payload, frame + sizeof(message_header_t), out_header->payload_size);
    }

    return STATUS_OK;
}

void pipe_close(named_pipe_t *pipe) {
    if (!pipe) {
        return;
    }

    if (pipe->handle != INVALID_HANDLE_VALUE) {
        if (!CloseHandle(pipe->handle)) {
            pipe_log_win_error("CloseHandle(named pipe)");
        }
        pipe->handle = INVALID_HANDLE_VALUE;
    }

    free(pipe);
}
