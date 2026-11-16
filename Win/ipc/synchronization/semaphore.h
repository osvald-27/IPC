#ifndef SEMAPHORE_H
#define SEMAPHORE_H

#include "../common/status.h"

typedef struct transaction_lock transaction_lock_t;

status_t semaphore_create(const char *name, transaction_lock_t **lock);
status_t semaphore_open(const char *name, transaction_lock_t **lock);
status_t semaphore_lock(transaction_lock_t *lock);
status_t semaphore_unlock(transaction_lock_t *lock);
void semaphore_close(transaction_lock_t *lock);
status_t semaphore_remove(transaction_lock_t *lock);

#endif /* SEMAPHORE_H */