#ifndef NAMED_PIPE_H
#define NAMED_PIPE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#include "../common/status.h"

#define CROSSIPC_PIPE_NAME "\\\\.\\pipe\\CrossIPC"
#define CROSSIPC_PIPE_BUFFER 8192U

status_t named_pipe_server_create(HANDLE *pipe_handle);
status_t named_pipe_server_accept(HANDLE pipe_handle);
status_t named_pipe_client_connect(HANDLE *pipe_handle);
status_t named_pipe_read_exact(HANDLE pipe_handle, void *buffer, size_t length);
status_t named_pipe_write_exact(HANDLE pipe_handle, const void *buffer, size_t length);
void named_pipe_disconnect(HANDLE pipe_handle);
void named_pipe_server_disconnect(HANDLE pipe_handle);

#endif
