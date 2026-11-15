#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "../common/status.h"

/*
 * Same interface as Lin/ipc/synchronization/semaphore.h so that
 * server/transaction.c locks the same way on both systems.
 * On Windows the lock is a named semaphore object with a count of 1.
 *
 * key_path is used as the object name (for example SEM_NAME from
 * protocol.h). proj_id is kept only to match the Linux signature,
 * Windows has no ftok() so it is ignored.
 */
typedef struct transaction_lock transaction_lock_t;

/* Server only. Fails with STATUS_ALREADY_EXISTS if the name is taken. */
status_t semaphore_create(const char *key_path, char proj_id, transaction_lock_t **lock);

/* Workers open the semaphore the server created. */
status_t semaphore_open(const char *key_path, char proj_id, transaction_lock_t **lock);

/* P operation. Blocks until the lock is free. */
status_t semaphore_lock(transaction_lock_t *lock);

/* V operation. */
status_t semaphore_unlock(transaction_lock_t *lock);

/* Closes this process's handle only. */
void semaphore_close(transaction_lock_t *lock);

/*
 * Windows has no call that deletes a named object. The kernel frees
 * it when the last handle is closed, so this closes the handle and
 * the object disappears once every worker has done the same.
 */
status_t semaphore_remove(transaction_lock_t *lock);

#endif /* SEMAPHORE_H */
