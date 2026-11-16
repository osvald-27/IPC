/*
 * shared_memory.c
 *
 * Manages System V shared memory segments on Linux (ftok, shmget, shmat, shmdt,
 * shmctl) and provides safe snapshot publishing/reading helpers.
 *
 * Why this wrapper exists:
 * Raw System V shared-memory syscalls use awkward error sentinels—shmget()
 * returns -1, while shmat() returns (void *)-1 instead of NULL—and require
 * careful permission flag handling. Wrapping them behind status_t keeps the
 * rest of the server and client clean, consistent, and safe from leaks.
 */

#include "shared_memory.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/*
 * Converts the shared anchor file path on disk and a single project character
 * ('S' or 'P') into a reproducible System V key_t via ftok().
 *
 * Why ftok() is used instead of hardcoding a random integer:
 * ftok() hashes the file's filesystem inode and device number together with
 * proj_id. As long as both the server and client point to the same file, they
 * independently compute the exact same kernel key without exchanging IDs first.
 */
static key_t derive_shm_key(const char *key_path, char proj_id) {
    /* Use the standard utilities/ipc.key path if the caller didn't pass a custom one */
    const char *path = (key_path != NULL) ? key_path : DEFAULT_SHM_KEY_FILE;
    key_t key = ftok(path, proj_id);

    /*
     * Convenience fallback: if someone runs the client binary directly from inside
     * client/ or shared_memory/ instead of Lin/ipc/, check one directory higher
     * before failing.
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
 * Allocates a brand-new shared-memory segment in the kernel and attaches it
 * with full read/write permissions for the server.
 */
status_t shm_create(const char *key_path, char proj_id, size_t size, int *shm_id, void **addr) {
    /* Validate pointers and size up front so we never pass 0 bytes to shmget() */
    if (size == 0 || shm_id == NULL || addr == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    key_t key = derive_shm_key(key_path, proj_id);
    if (key == (key_t)-1) {
        return STATUS_SHARED_MEMORY_ERROR;
    }

    /*
     * Flags explained:
     *   - IPC_CREAT : Ask the kernel to allocate a new segment if none exists.
     *   - IPC_EXCL  : Fail with EEXIST if a segment for this key is already in
     *                 the kernel. This stops the server from silently adopting
     *                 stale, half-corrupted memory left behind by an earlier crash.
     *   - 0600      : Owner read/write only—no other user on the Linux machine can
     *                 inspect or attach to our banking memory.
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
     * Map the kernel segment into this process's virtual address space:
     *   - shmaddr = NULL : Let the Linux kernel pick an aligned virtual address.
     *   - shmflg  = 0    : Map with both read and write permissions enabled.
     *
     * Note the quirk: shmat() returns (void *)-1 on error, NOT NULL. If mapping
     * fails, we immediately delete the segment with IPC_RMID so we don't leak
     * orphaned IPC memory in the kernel.
     */
    void *mapped = shmat(id, NULL, 0);
    if (mapped == (void *)-1) {
        perror("shmat (create) failed");
        shmctl(id, IPC_RMID, NULL);
        return STATUS_SHARED_MEMORY_ERROR;
    }

    /* Zero out newly mapped pages so uninitialized bytes never leak */
    memset(mapped, 0, size);

    *shm_id = id;
    *addr   = mapped;
    return STATUS_OK;
}

/*
 * Attaches an existing shared-memory segment into the calling process.
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
     * Notice IPC_CREAT is intentionally left out here: if the server isn't running
     * yet, shmget() fails with ENOENT so the client can report STATUS_NOT_FOUND
     * cleanly instead of accidentally creating an empty, uninitialized segment.
     * Passing size = 0 tells shmget() to accept whatever size the server created.
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
     * Enforcing least privilege at the hardware page-table level:
     * When read_only is non-zero, SHM_RDONLY tells the kernel to mark these
     * virtual memory pages read-only in the client's MMU page table. Even if
     * buggy or malicious client code casts away 'const' and tries to write to
     * a balance, the CPU immediately raises a SIGSEGV protection fault.
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
 * Detaches the shared-memory segment from this process's virtual address space.
 * Calling shmdt() only lowers the kernel's attachment reference count; the
 * underlying memory stays alive for any other processes still attached.
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
 * Marks the shared-memory segment for destruction in the kernel during server
 * shutdown. Linux defers freeing the physical RAM pages until the last attached
 * process detaches or terminates, then cleans everything up automatically.
 */
status_t shm_remove(int shm_id) {
    if (shmctl(shm_id, IPC_RMID, NULL) == -1) {
        perror("shmctl (IPC_RMID) failed");
        return STATUS_SHARED_MEMORY_ERROR;
    }

    return STATUS_OK;
}

/*
 * Publishes sanitized account data into the read-only client snapshot using a
 * seqlock-style sequence counter so clients never read half-updated figures.
 *
 * How the sequence counter works:
 *   1. Before copying, increment sequence from even -> odd (signals "write in progress").
 *   2. Copy only the safe public fields from authoritative accounts into snapshot.
 *   3. After copying, increment sequence from odd -> even (signals "snapshot stable").
 *
 * Caller must already hold the server's transaction semaphore when calling this.
 */
void shm_publish_snapshot(bank_snapshot_t *snapshot, const account_t *accounts, uint32_t count) {
    if (snapshot == NULL || accounts == NULL) {
        return;
    }

    uint32_t limit = (count > SNAPSHOT_MAX_ACCOUNTS) ? SNAPSHOT_MAX_ACCOUNTS : count;

    /* Step 1: Mark snapshot dirty (odd sequence number) */
    snapshot->sequence++;
    __sync_synchronize();

    /* Step 2: Copy only permitted public fields into the snapshot */
    snapshot->account_count = limit;
    for (uint32_t i = 0; i < limit; i++) {
        snapshot->accounts[i].client_id         = accounts[i].client_id;
        snapshot->accounts[i].balance           = accounts[i].balance;
        snapshot->accounts[i].total_deposits    = accounts[i].total_deposits;
        snapshot->accounts[i].total_withdrawals = accounts[i].total_withdrawals;
        snapshot->accounts[i].version           = accounts[i].version;
    }

    /* Step 3: Mark snapshot clean again (even sequence number) */
    __sync_synchronize();
    snapshot->sequence++;
}

/*
 * Reads a single account's published state from the read-only client snapshot
 * without needing to acquire the server's write semaphore.
 *
 * To prevent a torn read (seeing a new balance paired with an old version), we
 * sample 'sequence' before and after copying the record. If sequence was odd
 * (server mid-write) or changed while we were copying, we spin and retry up
 * to a bounded limit.
 */
status_t shm_read_account_snapshot(const bank_snapshot_t *snapshot, const client_id_t *id, account_snapshot_t *out) {
    if (snapshot == NULL || id == NULL || out == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    for (int attempt = 0; attempt < 1000; attempt++) {
        uint64_t seq_before = snapshot->sequence;
        __sync_synchronize();

        /* Odd sequence number means the server is actively modifying the snapshot */
        if ((seq_before & 1ULL) != 0ULL) {
            continue;
        }

        uint32_t count = snapshot->account_count;
        if (count > SNAPSHOT_MAX_ACCOUNTS) {
            count = SNAPSHOT_MAX_ACCOUNTS;
        }

        account_snapshot_t temp;
        int found = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (strncmp(snapshot->accounts[i].client_id.value, id->value, CLIENT_ID_LENGTH) == 0) {
                temp  = snapshot->accounts[i];
                found = 1;
                break;
            }
        }

        __sync_synchronize();
        uint64_t seq_after = snapshot->sequence;

        /* If sequence didn't change while we read, our copy is 100% consistent */
        if (seq_before == seq_after) {
            if (!found) {
                return STATUS_ACCOUNT_NOT_FOUND;
            }
            *out = temp;
	    return STATUS_OK;
	}
    return STATUS_TIMEOUT;
    }
    return STATUS_TIMEOUT;
}
