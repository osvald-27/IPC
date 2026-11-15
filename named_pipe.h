#ifndef NAMED_PIPE_H
#define NAMED_PIPE_H

#include <stdint.h>
#include "../common/protocol.h"
#include "../common/status.h"
#include "../common/types.h"

/* ============================================================================
 * CrossIPC Windows Named Pipe Interface (Win32 API)
 * Owner: @William (Win/ipc/named_pipe)
 *
 * Wraps Win32 Named Pipes (CreateNamedPipeA, ConnectNamedPipe, CreateFileA,
 * ReadFile, WriteFile, DisconnectNamedPipe, CloseHandle) behind an opaque
 * named_pipe_t handle so the server and client modules exchange framed
 * protocol messages without dealing with raw Win32 HANDLEs directly.
 * ============================================================================ */

typedef struct named_pipe named_pipe_t;

/*
 * Logs the calling thread's GetLastError() code and human-readable
 * FormatMessageA() string to stderr.
 */
void pipe_log_win_error(const char *context);

/*
 * Server-side: Creates a new duplex, message-mode Win32 Named Pipe instance
 * (PIPE_UNLIMITED_INSTANCES) for `pipe_name` (e.g., PIPE_NAME).
 */
status_t pipe_server_create(const char *pipe_name, named_pipe_t **out_pipe);

/*
 * Server-side: Blocks waiting for a client to connect to the pipe instance.
 * Treats ERROR_PIPE_CONNECTED as a valid connection (STATUS_OK).
 */
status_t pipe_server_accept(named_pipe_t *pipe);

/*
 * Server-side: Flushes buffered replies and disconnects the current client
 * from this pipe instance.
 */
status_t pipe_server_disconnect(named_pipe_t *pipe);

/*
 * Client-side: Connects to an existing server named pipe, retrying with
 * WaitNamedPipeA() up to `timeout_ms` if all instances are temporarily busy.
 */
status_t pipe_client_open(const char *pipe_name,
                          uint32_t timeout_ms,
                          named_pipe_t **out_pipe);

/*
 * Sends one framed message (message_header_t + payload) atomically in
 * Win32 message mode.
 */
status_t pipe_send_message(named_pipe_t *pipe,
                           const message_header_t *header,
                           const void *payload);

/*
 * Receives one framed message, validating PROTOCOL_NUMBER, PROTOCOL_VERSION,
 * header_size, and payload bounds. Returns STATUS_QUEUE_EMPTY on clean EOF /
 * ERROR_BROKEN_PIPE.
 */
status_t pipe_recv_message(named_pipe_t *pipe,
                           message_header_t *out_header,
                           void *out_payload,
                           uint32_t max_payload_size);

/*
 * Closes the underlying Win32 HANDLE and frees the named_pipe_t struct.
 */
void pipe_close(named_pipe_t *pipe);

#endif /* NAMED_PIPE_H */
