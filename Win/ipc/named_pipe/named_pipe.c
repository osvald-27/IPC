#include "named_pipe.h"

#include <stdio.h>
#include <stdlib.h>

#define PIPE_BUFFER_SIZE 8192

struct pipe_conn {
    HANDLE pipe;
    HANDLE io_event;   /* signalled when an overlapped operation finishes */
};

static status_t make_conn(HANDLE pipe, pipe_conn_t **conn) {
    pipe_conn_t *c = malloc(sizeof *c);
    if (c == NULL) {
        CloseHandle(pipe);
        return STATUS_OUT_OF_MEMORY;
    }
    c->pipe = pipe;
    c->io_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (c->io_event == NULL) {
        fprintf(stderr, "CreateEvent failed: %lu\n", GetLastError());
        CloseHandle(pipe);
        free(c);
        return STATUS_QUEUE_ERROR;
    }
    *conn = c;
    return STATUS_OK;
}

/*
 * The pipe is opened with FILE_FLAG_OVERLAPPED, so ReadFile, WriteFile
 * and ConnectNamedPipe return at once and finish later. This waits for
 * the operation or for the stop event, whichever comes first.
 */
static status_t finish_io(pipe_conn_t *c, OVERLAPPED *ov, HANDLE stop, DWORD *done) {
    HANDLE waits[2] = { ov->hEvent, stop };
    DWORD count = (stop != NULL) ? 2 : 1;

    DWORD r = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0 + 1) {
        CancelIo(c->pipe);
        GetOverlappedResult(c->pipe, ov, done, TRUE);   /* let the cancel settle */
        return STATUS_SHUTTING_DOWN;
    }
    if (r != WAIT_OBJECT_0) {
        fprintf(stderr, "wait on pipe failed: %lu\n", GetLastError());
        return STATUS_QUEUE_ERROR;
    }
    if (!GetOverlappedResult(c->pipe, ov, done, FALSE)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA) {
            return STATUS_QUEUE_EMPTY;
        }
        fprintf(stderr, "pipe I/O failed: %lu\n", err);
        return STATUS_QUEUE_ERROR;
    }
    return STATUS_OK;
}

/*
 * A pipe is a byte stream. One ReadFile may return less than we asked
 * for, so loop until the whole buffer has moved.
 */
static status_t transfer_all(pipe_conn_t *c, void *buffer, DWORD length, int writing, HANDLE stop) {
    char *p = buffer;
    DWORD moved = 0;

    while (moved < length) {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = c->io_event;
        ResetEvent(c->io_event);

        DWORD done = 0;
        BOOL ok = writing ? WriteFile(c->pipe, p + moved, length - moved, NULL, &ov)
                          : ReadFile(c->pipe, p + moved, length - moved, NULL, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA) {
                return STATUS_QUEUE_EMPTY;
            }
            if (err != ERROR_IO_PENDING) {
                fprintf(stderr, "%s failed: %lu\n", writing ? "WriteFile" : "ReadFile", err);
                return STATUS_QUEUE_ERROR;
            }
        }

        status_t st = finish_io(c, &ov, stop, &done);
        if (st != STATUS_OK) {
            return st;
        }
        if (done == 0) {
            return STATUS_QUEUE_EMPTY;
        }
        moved += done;
    }
    return STATUS_OK;
}

status_t pipe_server_create(pipe_conn_t **conn) {
    if (conn == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /*
     * Every worker process calls this with the same name. Each call adds
     * one more instance of the pipe, and a client is given to whichever
     * instance is waiting in ConnectNamedPipe.
     */
    HANDLE pipe = CreateNamedPipeA(PIPE_NAME,
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   PIPE_UNLIMITED_INSTANCES,
                                   PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateNamedPipe failed: %lu\n", GetLastError());
        return STATUS_QUEUE_ERROR;
    }
    return make_conn(pipe, conn);
}

status_t pipe_server_accept(pipe_conn_t *conn, HANDLE stop) {
    if (conn == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof ov);
    ov.hEvent = conn->io_event;
    ResetEvent(conn->io_event);

    if (ConnectNamedPipe(conn->pipe, &ov)) {
        return STATUS_OK;
    }
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        /* the client connected between CreateNamedPipe and this call */
        return STATUS_OK;
    }
    if (err != ERROR_IO_PENDING) {
        fprintf(stderr, "ConnectNamedPipe failed: %lu\n", err);
        return STATUS_QUEUE_ERROR;
    }

    DWORD ignored = 0;
    return finish_io(conn, &ov, stop, &ignored);
}

void pipe_server_disconnect(pipe_conn_t *conn) {
    if (conn == NULL) {
        return;
    }
    FlushFileBuffers(conn->pipe);
    DisconnectNamedPipe(conn->pipe);
}

status_t pipe_client_connect(pipe_conn_t **conn, DWORD timeout_ms) {
    if (conn == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    DWORD start = GetTickCount();
    for (;;) {
        HANDLE pipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (pipe != INVALID_HANDLE_VALUE) {
            return make_conn(pipe, conn);
        }

        DWORD err = GetLastError();
        DWORD waited = GetTickCount() - start;
        if (waited >= timeout_ms) {
            return err == ERROR_PIPE_BUSY ? STATUS_TIMEOUT : STATUS_SERVER_UNAVAILABLE;
        }
        if (err == ERROR_PIPE_BUSY) {
            /* every instance has a client, wait for one to come free */
            WaitNamedPipeA(PIPE_NAME, timeout_ms - waited);
        } else if (err == ERROR_FILE_NOT_FOUND) {
            /* server not started yet */
            Sleep(50);
        } else {
            fprintf(stderr, "CreateFile on pipe failed: %lu\n", err);
            return STATUS_QUEUE_ERROR;
        }
    }
}

status_t pipe_send(pipe_conn_t *conn, const message_header_t *header, const void *payload, HANDLE stop) {
    if (conn == NULL || header == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (header->payload_size > MAX_PAYLOAD_SIZE || (header->payload_size > 0 && payload == NULL)) {
        return STATUS_INVALID_ARGUMENT;
    }

    status_t st = transfer_all(conn, (void *)header, sizeof *header, 1, stop);
    if (st != STATUS_OK || header->payload_size == 0) {
        return st;
    }
    return transfer_all(conn, (void *)payload, header->payload_size, 1, stop);
}

status_t pipe_recv(pipe_conn_t *conn, message_header_t *header, void *payload, uint32_t capacity, HANDLE stop) {
    if (conn == NULL || header == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    status_t st = transfer_all(conn, header, sizeof *header, 0, stop);
    if (st != STATUS_OK) {
        return st;
    }

    /* reject anything that is not our protocol before trusting the sizes */
    if (header->magic != PROTOCOL_NUMBER || header->version != PROTOCOL_VERSION ||
        header->header_size != sizeof *header) {
        return STATUS_QUEUE_ERROR;
    }
    if (header->payload_size > capacity || header->payload_size > MAX_PAYLOAD_SIZE) {
        return STATUS_QUEUE_ERROR;
    }
    if (header->payload_size == 0) {
        return STATUS_OK;
    }
    return transfer_all(conn, payload, header->payload_size, 0, stop);
}

void pipe_close(pipe_conn_t *conn) {
    if (conn == NULL) {
        return;
    }
    CloseHandle(conn->io_event);
    CloseHandle(conn->pipe);
    free(conn);
}
