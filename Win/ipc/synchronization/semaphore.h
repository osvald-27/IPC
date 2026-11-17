#ifndef SEMAPHORE_H
#define SEMAPHORE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../common/status.h"
typedef struct { HANDLE handle; char name[128]; } transaction_lock_t;
status_t semaphore_create(const char *name,transaction_lock_t **lock);
status_t semaphore_open(const char *name,transaction_lock_t **lock);
status_t semaphore_lock(transaction_lock_t *lock);
status_t semaphore_unlock(transaction_lock_t *lock);
void semaphore_remove(transaction_lock_t *lock);
#endif
