<<<<<<< HEAD
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
=======
/* shared_memory.h - Windows shared-memory adapter (counterpart of the Linux shmget/shmat code).
 *
 * Two regions, as in the design (Sections 5.2 and 13.2.1):
 *   AUTH : authoritative accounts. Server and its workers only. Clients never open it.
 *   SNAP : small public snapshot. Server writes it, clients open it READ-ONLY.
 *
 * Rules:
 *  - No pointers inside a region. The view is mapped at a different address in every process,
 *    so records are referenced by index or offset.
 *  - The region is NOT the source of truth: the persistence layer fills AUTH at startup.
 *  - Windows frees a region when the LAST handle closes, so there is no "remove" call.
 */
#ifndef CROSSIPC_SHARED_MEMORY_H
#define CROSSIPC_SHARED_MEMORY_H

#include <stddef.h>

#define SHM_NAME_AUTH "Local\\CrossIPC_Auth"
#define SHM_NAME_SNAP "Local\\CrossIPC_Snap"

typedef enum { SHM_OK = 0, SHM_EXISTS = 1, SHM_NOT_FOUND = 2, SHM_ERROR = -1 } ShmStatus;

typedef struct {
    void  *handle;      /* file-mapping handle   */
    void  *view;        /* address in THIS process */
    size_t size;
    int    read_only;   /* 1 if opened with shm_open_ro */
} ShmRegion;

/* Server: create a zero-filled region. SHM_EXISTS if the name is already in use (another
 * server, or a stale region kept alive by a client handle). Never reuses or wipes it. */
ShmStatus shm_create(const char *name, size_t size, ShmRegion *out);

/* Workers: open an existing region read-write. SHM_NOT_FOUND if the server has not created it. */
ShmStatus shm_open_rw(const char *name, size_t size, ShmRegion *out);

/* Clients: open an existing region READ-ONLY (FILE_MAP_READ). Writing through it crashes
 * the writer with an access violation (0xC0000005). */
ShmStatus shm_open_ro(const char *name, size_t size, ShmRegion *out);

/* Unmap and close. Safe to call twice. */
void shm_close(ShmRegion *r);

/* ------------------------------------------------------------------------------------
 * Snapshot publication (consistent reads without a lock, a "seqlock").
 *
 * A reader cannot take the server's lock (that would let any client stall the server), so
 * the snapshot carries a sequence number: odd while the server is writing, even when stable.
 * The reader copies the payload and re-reads the number; if it changed, it retries.
 * The payload is an opaque block of bytes chosen by the group (fields: open question Q5).
 * ------------------------------------------------------------------------------------ */
typedef enum { SNAP_OK = 0, SNAP_EMPTY = 1, SNAP_BUSY = 2, SNAP_ERROR = -1 } SnapStatus;

/* Region size needed for a payload of payload_size bytes (header + payload). */
size_t snap_region_size(size_t payload_size);

/* Server only (read-write region). Publishes a complete new payload. */
SnapStatus snap_publish(ShmRegion *r, const void *payload, size_t len);

/* Any process. SNAP_EMPTY: nothing published yet. SNAP_BUSY: gave up after max_tries. */
SnapStatus snap_read(const ShmRegion *r, void *out, size_t len, int max_tries);

#endif
>>>>>>> origin/main
