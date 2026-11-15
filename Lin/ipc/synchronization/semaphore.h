#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "../common/status.h"

/*
 * Opaque handle for the global transaction lock.
 * Hiding the underlying OS semaphore ID behind this pointer lets
 * server/transaction.c call the exact same locking functions on
 * both Linux and Windows without caring how the kernel implements it.
 */
typedef struct transaction_lock transaction_lock_t;

/*
 * Creates and initializes a new binary semaphore set to 1 (unlocked).
 * Called once by the main server during startup using a key derived
 * from key_path.
 */
status_t semaphore_create(const char *key_path, char proj_id, transaction_lock_t **lock);

/*
 * Attaches to an existing semaphore created by the server so other
 * processes can coordinate access without recreating the lock.
 */
status_t semaphore_open(const char *key_path, char proj_id, transaction_lock_t **lock);

/*
 * Acquires the transaction lock (P operation). Blocks the calling
 * process until no other worker is modifying balances or writing logs.
 */
status_t semaphore_lock(transaction_lock_t *lock);

/*
 * Releases the transaction lock (V operation) so the next waiting
 * worker process can enter the critical section.
 */
status_t semaphore_unlock(transaction_lock_t *lock);

/*
 * Frees the local handle memory without deleting the semaphore from
 * the kernel. Used by worker processes when they exit.
 */
void semaphore_close(transaction_lock_t *lock);

/*
 * Destroys the semaphore set in the kernel and frees the handle.
 * Called only by the main server during shutdown.
 */
status_t semaphore_remove(transaction_lock_t *lock);

#endif /* SEMAPHORE_H */
