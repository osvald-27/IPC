#ifndef NAMED_PIPE_H
#define NAMED_PIPE_H

#include <windows.h>

#include "../common/protocol.h"

/*
 * Windows counterpart of msg_q/. One pipe_conn_t is one end of the
 * named pipe PIPE_NAME. Every message is a message_header_t followed
 * by header.payload_size bytes.
 *
 * All calls take a stop event. When it is signalled, a blocked call
 * gives up and returns STATUS_SHUTTING_DOWN, which is how Ctrl+C gets
 * a process out of a wait. Pass NULL to wait with no way out.
 */
typedef struct pipe_conn pipe_conn_t;

/* Server side: create one instance of the pipe. */
status_t pipe_server_create(pipe_conn_t **conn);

/* Server side: wait for a client to connect to this instance. */
status_t pipe_server_accept(pipe_conn_t *conn, HANDLE stop);

/* Server side: drop the current client so the instance can be reused. */
void pipe_server_disconnect(pipe_conn_t *conn);

/* Client side: open the pipe, waiting up to timeout_ms for a free instance. */
status_t pipe_client_connect(pipe_conn_t **conn, DWORD timeout_ms);

/* Send header then payload. header->payload_size says how many payload bytes. */
status_t pipe_send(pipe_conn_t *conn, const message_header_t *header, const void *payload, HANDLE stop);

/*
 * Receive one message. Returns STATUS_QUEUE_EMPTY when the other side
 * has closed the pipe, which is the normal end of a conversation.
 */
status_t pipe_recv(pipe_conn_t *conn, message_header_t *header, void *payload, uint32_t capacity, HANDLE stop);

void pipe_close(pipe_conn_t *conn);

#endif /* NAMED_PIPE_H */
