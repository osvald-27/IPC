#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include "../common/protocol.h"

/*
 * The bank (bank_t from protocol.h) lives in a named file mapping
 * backed by the page file. The server creates it, the workers open
 * it read-write, and a client may open it read-only.
 */
typedef struct shared_memory shared_memory_t;

/* Server only. Creates the mapping, zero-filled by Windows. */
status_t shared_memory_create(shared_memory_t **shm);

/* read_only != 0 maps the view with FILE_MAP_READ, any write then faults. */
status_t shared_memory_open(shared_memory_t **shm, int read_only);

/* Address of the bank inside this process. Never send it to another process. */
bank_t *shared_memory_bank(shared_memory_t *shm);

/* Unmaps the view and closes the handle. */
void shared_memory_close(shared_memory_t *shm);

#endif /* SHARED_MEMORY_H */
