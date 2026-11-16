/*
 * shared_memory.c
 *
 * Wraps Linux System V shared memory syscalls (ftok, shmget, shmat, shmdt, shmctl)
 * behind status_t return codes so the server and client never have to deal with raw
 * void* error sentinels like (void *)-1 or manual permission flags directly.
 */

#include "shared_memory.h"

#include <errno.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/*
 * Derives a reproducible System V IPC key from the shared anchor file on disk
 * and project character ('S'). Both the server and client run this exact same
 * lookup so they attach to the exact same kernel memory segment without passing
 * raw IDs over command-line arguments.
 */
static key_t derive_shm_key(const char *key_path, char proj_id) {
    /* Fall back to the default utilities/ipc.key file if the caller passes NULL */
    const char *path = (key_path != NULL) ? key_path : DEFAULT_SHM_KEY_FILE;
    key_t key = ftok(path, proj_id);

    /* If someone launches the client binary from inside a subdirectory like client/
     * instead of the project root, check one level up before giving up.
     */
    if (key == (key_t)-1 && key_path == NULL) {
        key = ftok("../utilities/ipc.key", proj_id);
    }

    if (key == (key_t)-1) {
        perror("ftok failed for shared memory");
    }

    return key;
}

/*
 * Allocates a brand-new shared-memory segment in the kernel and maps it into the
 * server's virtual address space with full read/write permissions.
 */
status_t shm_create(const char *key_path, char proj_id, size_t size, int *shm_id, void **addr) {
    /* Reject zero-byte requests or NULL output pointers before touching the kernel */
    if (size == 0 || shm_id == NULL || addr == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    key_t key = derive_shm_key(key_path, proj_id);
    if (key == (key_t)-1) {
        return STATUS_SHARED_MEMORY_ERROR;
    }

    /*
     * IPC_CREAT | IPC_EXCL forces shmget() to fail with EEXIST if a leftover
     * shared-memory segment from an earlier crash is still sitting in the kernel,
     * preventing the server from silently adopting stale balances. Mode 0600
     * restricts access strictly to the user running the server.
     */
    int id = shmget(key, size, IPC_CREAT | IPC_EXCL | 0600);
    if (id == -1) {
        perror("shmget (create) failed");
        if (errno == EEXIST) {
            return STATUS_ALREADY_EXISTS;
        }
        if (errno == EACCES) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_SHARED_MEMORY_ERROR;
    }

    /*
     * Map the newly created segment into our virtual address space:
     *   - shmaddr = NULL lets the kernel choose a safe page-aligned address
     *   - shmflg  = 0 gives full read/write access (needed by the server)
     *
     * Note that shmat() returns (void *)-1 on failure rather than NULL. If mapping
     * fails, immediately destroy the segment with IPC_RMID so we don't leak kernel RAM.
     */
    void *mapped = shmat(id, NULL, 0);
    if (mapped == (void *)-1) {
        perror("shmat (create) failed");
        shmctl(id, IPC_RMID, NULL);
        return STATUS_SHARED_MEMORY_ERROR;
    }

    *shm_id = id;
    *addr   = mapped;
    return STATUS_OK;
}

/*
 * Attaches a client (or secondary tool) to an existing shared-memory segment
 * created by the server.
 */
status_t shm_open(const char *key_path, char proj_id, size_t size, int read_only, int *shm_id, void **addr) {
    if (shm_id == NULL || addr == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    key_t key = derive_shm_key(key_path, proj_id);
    if (key == (key_t)-1) {
        return STATUS_SHARED_MEMORY_ERROR;
    }

    /*
     * Intentionally omit IPC_CREAT here so clients fail cleanly with STATUS_NOT_FOUND
     * (ENOENT) if the server hasn't started yet, rather than accidentally creating
     * an uninitialized segment. Passing size = 0 tells shmget() to attach to whatever
     * size the server originally allocated.
     */
    int id = shmget(key, size, 0600);
    if (id == -1) {
        perror("shmget (open) failed");
        if (errno == ENOENT) {
            return STATUS_NOT_FOUND;
        }
        if (errno == EACCES) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_SHARED_MEMORY_ERROR;
    }

    /*
     * When read_only is non-zero, SHM_RDONLY instructs the CPU's memory management
     * unit (MMU) to mark these pages read-only in this process's page table. Any
     * attempt by a client to write to the mapped struct triggers an immediate SIGSEGV
     * hardware fault instead of silently corrupting server data.
     */
    int flags = read_only ? SHM_RDONLY : 0;
    void *mapped = shmat(id, NULL, flags);
    if (mapped == (void *)-1) {
        perror("shmat (open) failed");
        return STATUS_SHARED_MEMORY_ERROR;
    }

    *shm_id = id;
    *addr   = mapped;
    return STATUS_OK;
}

/*
 * Unmaps the shared-memory segment from the calling process's virtual address space.
 * This only decrements the kernel's attachment count—the physical pages stay intact
 * for other attached processes until shm_remove() is called.
 */
status_t shm_detach(const void *addr) {
    if (addr == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (shmdt(addr) == -1) {
        perror("shmdt failed");
        return STATUS_SHARED_MEMORY_ERROR;
    }

    return STATUS_OK;
}

/*
 * Marks the shared-memory segment for deletion in the kernel during server shutdown.
 * Linux keeps the segment alive until every process that called shmat() detaches or
 * exits, then automatically frees the underlying physical memory pages.
 */
status_t shm_remove(int shm_id) {
    if (shmctl(shm_id, IPC_RMID, NULL) == -1) {
        perror("shmctl (IPC_RMID) failed");
        return STATUS_SHARED_MEMORY_ERROR;
    }

    return STATUS_OK;
}
