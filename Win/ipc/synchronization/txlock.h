/* txlock.h - low-level Windows transaction lock (named mutex).
 *
 * Counterpart of the Linux System V semaphore (Lin/ipc/synchronization/semaphore.h).
 * The group-facing interface (semaphore.h) is a thin wrapper over this file, so the server can
 * call the same semaphore_* functions on both platforms.
 *
 * DESIGN DECISION: the primitive underneath is a named Windows MUTEX, not a Windows
 * semaphore. A Windows semaphore has no owner and no equivalent of SEM_UNDO: if a worker
 * dies while holding it, the lock stays closed forever and nobody is told. A mutex has an
 * owner, so Windows reports "the owner died" (WAIT_ABANDONED) to the next process that
 * waits. We surface that to the caller as TXLOCK_ABANDONED.
 *
 * IMPORTANT: TXLOCK_ABANDONED means "you DO hold the lock, but the previous owner died
 * in the middle of a transaction". The caller must repair the shared data (roll back the
 * in-flight transaction) and then release the lock as usual.
 *
 * Function names use the txlock_ prefix so they never clash with the POSIX sem_* names
 * of MinGW's own <semaphore.h>.
 */
#ifndef CROSSIPC_TXLOCK_H
#define CROSSIPC_TXLOCK_H

#include <stdint.h>

#define TXLOCK_NAME         "Local\\CrossIPC_TxLock"
#define TXLOCK_WAIT_FOREVER 0xFFFFFFFFu

typedef enum {
    TXLOCK_OK        =  0,   /* lock acquired / operation done                          */
    TXLOCK_ABANDONED =  1,   /* lock acquired, BUT the previous owner died holding it   */
    TXLOCK_TIMEOUT   =  2,   /* not acquired within timeout_ms                          */
    TXLOCK_EXISTS    =  3,   /* create only: the name is already in use                 */
    TXLOCK_NOT_FOUND =  4,   /* open only: no such lock (server not started?)           */
    TXLOCK_DENIED    =  5,   /* open only: Windows refused access                       */
    TXLOCK_ERROR     = -1    /* system call failed (message printed on stderr)          */
} TxLockStatus;

typedef struct { void *handle; } TxLock;

/* Server: create the named lock. Never silently reuses an existing one. */
TxLockStatus txlock_create(const char *name, TxLock *out);

/* Workers: open the lock that the server created. */
TxLockStatus txlock_open(const char *name, TxLock *out);

/* Wait for the lock. Always pass a finite timeout in server code. */
TxLockStatus txlock_acquire(TxLock *l, uint32_t timeout_ms);

/* Release. Returns TXLOCK_ERROR if the caller does not own the lock (a real bug). */
TxLockStatus txlock_release(TxLock *l);

/* Close the handle. Windows deletes the lock when the last handle is closed. */
void txlock_close(TxLock *l);

#endif
