#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stddef.h>
#include <sys/types.h>

#include "../common/types.h"
#include "../common/status.h"

/* Must match server.h so server and clients derive the exact same System V key */
#define DEFAULT_SHM_KEY_FILE "utilities/ipc.key"
#define SHM_PROJ_ID          'S'

/*
 * Creates a brand-new System V shared-memory segment of 'size' bytes with
 * owner-only permissions (0600) and attaches it with read/write access.
 * Called once by the server during startup.
 */
status_t shm_create(const char *key_path, char proj_id, size_t size, int *shm_id, void **addr);

/*
 * Attaches to an existing shared-memory segment created by the server.
 * When read_only is non-zero, the segment is mapped with SHM_RDONLY so the
 * operating system hardware-enforces that clients can inspect published
 * balances without ever modifying memory directly.
 */
status_t shm_open(const char *key_path, char proj_id, size_t size, int read_only, int *shm_id, void **addr);

/*
 * Detaches the shared-memory segment from the calling process's virtual
 * address space without deleting the underlying kernel segment.
 */
status_t shm_detach(const void *addr);

/*
 * Marks the shared-memory segment for destruction in the kernel during
 * server shutdown. The kernel frees the physical pages once every attached
 * process detaches.
 */
status_t shm_remove(int shm_id);

#endif /* SHARED_MEMORY_H */
#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stddef.h>
#include <sys/types.h>

#include "../common/types.h"
#include "../common/status.h"

/*
 * Both the server and client pass this same anchor file and project character ('S' for
 * Shared Memory) into ftok() so the kernel generates the exact same System V key_t
 * without needing to pass raw shared-memory IDs over the command line. Using 'S'
 * keeps this key separate from the message queue ('Q') and semaphore ('M').
 */
#define DEFAULT_SHM_KEY_FILE "utilities/ipc.key"
#define SHM_PROJ_ID          'S'

/*
 * Important rule for anything stored inside this shared segment:
 * Because shmat() maps the same physical RAM pages at completely different virtual
 * addresses in each process, raw pointers inside shared structs will crash other
 * processes with a segfault. Everything placed here must be flat, fixed-size arrays
 * or relative offsets.
 */

/*
 * Allocates a brand-new System V shared-memory segment of 'size' bytes with owner-only
 * permissions (0600) and maps it into the server's address space for reading and writing.
 * Under the hood, shmget() uses IPC_CREAT | IPC_EXCL so startup fails cleanly if a
 * stale segment from an earlier crash is still lingering in the kernel.
 */
status_t shm_create(const char *key_path, char proj_id, size_t size, int *shm_id, void **addr);

/*
 * Looks up an existing shared-memory segment created by the server (without IPC_CREAT,
 * so it fails cleanly with STATUS_NOT_FOUND if the server isn't up yet) and maps it
 * into the caller's virtual address space.
 *
 * When read_only is non-zero, shmat() passes SHM_RDONLY so the CPU's memory management
 * unit (MMU) marks those pages read-only—meaning any attempt by a client to write to
 * the mapping triggers an immediate SIGSEGV instead of corrupting bank balances.
 */
status_t shm_open(const char *key_path, char proj_id, size_t size, int read_only, int *shm_id, void **addr);

/*
 * Unmaps the segment from the calling process's virtual address space via shmdt().
 * Detaching only lowers the kernel's attachment reference count; the shared memory
 * stays alive in the kernel for any other running processes until shm_remove() is called.
 */
status_t shm_detach(const void *addr);

/*
 * Marks the segment for destruction in the kernel via shmctl(..., IPC_RMID, NULL)
 * during server shutdown. Linux won't actually reclaim the physical RAM until the
 * last attached process calls shm_detach() or exits, preventing mid-read crashes.
 */
status_t shm_remove(int shm_id);

#endif /* SHARED_MEMORY_H */
