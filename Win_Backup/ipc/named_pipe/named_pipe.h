#ifndef WIN_NAMED_PIPE_H
#define WIN_NAMED_PIPE_H
#include <stddef.h>
#include "../common/win_protocol.h"
typedef void *named_pipe_handle_t;
int named_pipe_connect(const char *name, named_pipe_handle_t *out);
int named_pipe_exchange(named_pipe_handle_t pipe, const Message *request, Message *response);
void named_pipe_close(named_pipe_handle_t pipe);
#endif
