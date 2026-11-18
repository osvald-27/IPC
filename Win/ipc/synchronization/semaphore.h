/* semaphore.h - group-facing transaction lock interface (Windows).
 *
 * Same function names and types as Lin/ipc/synchronization/semaphore.h, so
 * server/transaction.c, server.c and operator.c call the lock identically on both platforms.
 *
 * Differences from the Linux header, on purpose:
 *   - semaphore_create/open take only a NAME ("Local\\BankSemaphore"). Linux needs
 *     key_path + proj_id for ftok(); Windows objects are found by name.
 *   - semaphore_remove only closes this handle. Windows deletes the object itself when the
 *     LAST handle closes, so there is no IPC_RMID equivalent.
 *   - Underneath is a named MUTEX (not a Windows semaphore) so a dead owner is detected
 *     (see txlock.h). semaphore_lock never waits forever: after 10 s it returns STATUS_TIMEOUT.
 */
#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include <stdint.h>
#include "../common/status.h"

typedef struct transaction_lock transaction_lock_t;

/* Creates the lock. STATUS_ALREADY_EXISTS if the name is in use (caller may semaphore_open). */
status_t semaphore_create(const char *name, transaction_lock_t **lock);

/* Attaches to a lock created earlier. STATUS_NOT_FOUND if it does not exist. */
status_t semaphore_open(const char *name, transaction_lock_t **lock);

/* Acquire (blocks up to 10 s, then STATUS_TIMEOUT). Release with semaphore_unlock from the
 * SAME thread: a Windows mutex is owned by the thread that took it. */
status_t semaphore_lock(transaction_lock_t *lock);

/* Same, with your own timeout. *abandoned (optional) is set to 1 if the previous owner died
 * while holding the lock: you hold it, but must repair the data (roll back) before releasing. */
status_t semaphore_lock_timeout(transaction_lock_t *lock, uint32_t timeout_ms, int *abandoned);

status_t semaphore_unlock(transaction_lock_t *lock);

/* Close this process's handle and free the wrapper. */
void semaphore_close(transaction_lock_t *lock);

/* Server shutdown. On Windows identical to close (the OS removes the object itself). */
status_t semaphore_remove(transaction_lock_t *lock);

#endif /* SEMAPHORE_H */
