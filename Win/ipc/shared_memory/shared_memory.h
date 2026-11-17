#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../common/protocol.h"
#include "../common/status.h"
status_t shared_memory_create(const char *name,HANDLE *handle,bank_t **bank);
status_t shared_memory_open(const char *name,HANDLE *handle,bank_t **bank);
void shared_memory_close(HANDLE handle,bank_t *bank);
#endif
