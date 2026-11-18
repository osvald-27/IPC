<<<<<<< HEAD
#include "semaphore.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

struct transaction_lock {
    HANDLE handle;
};

static status_t wrap_handle(HANDLE h, transaction_lock_t **lock) {
    transaction_lock_t *l = malloc(sizeof *l);
    if (l == NULL) {
        CloseHandle(h);
        return STATUS_OUT_OF_MEMORY;
    }
    l->handle = h;
    *lock = l;
    return STATUS_OK;
}

status_t semaphore_create(const char *key_path, char proj_id, transaction_lock_t **lock) {
    (void)proj_id;
    if (key_path == NULL || lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* initial count 1, maximum 1: one holder at a time, like a mutex */
    HANDLE h = CreateSemaphoreA(NULL, 1, 1, key_path);
    if (h == NULL) {
        fprintf(stderr, "CreateSemaphore failed: %lu\n", GetLastError());
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    /*
     * CreateSemaphore succeeds even when the object already exists and
     * just hands back a handle to it. A leftover object means another
     * server is still running, so refuse to share it.
     */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return STATUS_ALREADY_EXISTS;
    }
    return wrap_handle(h, lock);
}

status_t semaphore_open(const char *key_path, char proj_id, transaction_lock_t **lock) {
    (void)proj_id;
    if (key_path == NULL || lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* SYNCHRONIZE to wait on it, SEMAPHORE_MODIFY_STATE to release it */
    HANDLE h = OpenSemaphoreA(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, key_path);
    if (h == NULL) {
        DWORD err = GetLastError();
        fprintf(stderr, "OpenSemaphore failed: %lu\n", err);
        if (err == ERROR_FILE_NOT_FOUND) {
            return STATUS_NOT_FOUND;
        }
        if (err == ERROR_ACCESS_DENIED) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_SYNCHRONIZATION_ERROR;
    }
    return wrap_handle(h, lock);
}

status_t semaphore_lock(transaction_lock_t *lock) {
    if (lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
    DWORD r = WaitForSingleObject(lock->handle, INFINITE);
    if (r != WAIT_OBJECT_0) {
        fprintf(stderr, "WaitForSingleObject on lock failed: %lu\n", GetLastError());
        return STATUS_SYNCHRONIZATION_ERROR;
    }
    return STATUS_OK;
}

status_t semaphore_unlock(transaction_lock_t *lock) {
    if (lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!ReleaseSemaphore(lock->handle, 1, NULL)) {
        fprintf(stderr, "ReleaseSemaphore failed: %lu\n", GetLastError());
        return STATUS_SYNCHRONIZATION_ERROR;
    }
    return STATUS_OK;
}

void semaphore_close(transaction_lock_t *lock) {
    if (lock == NULL) {
        return;
    }
    CloseHandle(lock->handle);
    free(lock);
}

status_t semaphore_remove(transaction_lock_t *lock) {
    if (lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
=======
/* semaphore.c - implements the group's semaphore_* interface on top of txlock (Windows). */
#include <stdio.h>
#include <stdlib.h>
#include "semaphore.h"
#include "txlock.h"

#define DEFAULT_LOCK_TIMEOUT_MS 10000u

struct transaction_lock {
    TxLock lock;
};

static status_t map_status(TxLockStatus s)
{
    switch (s) {
    case TXLOCK_OK:        return STATUS_OK;
    case TXLOCK_ABANDONED: return STATUS_OK;                 /* lock IS held; see semaphore_lock_timeout */
    case TXLOCK_TIMEOUT:   return STATUS_TIMED_OUT;
    case TXLOCK_EXISTS:    return STATUS_ALREADY_EXISTS;
    case TXLOCK_NOT_FOUND: return STATUS_NOT_FOUND;
    case TXLOCK_DENIED:    return STATUS_PERMISSION_DENIED;
    default:               return STATUS_SYNCHRONIZATION_ERROR;
    }
}

status_t semaphore_create(const char *name, transaction_lock_t **lock)
{
    if (name == NULL || lock == NULL) return STATUS_INVALID_ARGUMENT;
    transaction_lock_t *h = malloc(sizeof *h);
    if (h == NULL) return STATUS_OUT_OF_MEMORY;
    TxLockStatus s = txlock_create(name, &h->lock);
    if (s != TXLOCK_OK) { free(h); return map_status(s); }
    *lock = h;
    return STATUS_OK;
}

status_t semaphore_open(const char *name, transaction_lock_t **lock)
{
    if (name == NULL || lock == NULL) return STATUS_INVALID_ARGUMENT;
    transaction_lock_t *h = malloc(sizeof *h);
    if (h == NULL) return STATUS_OUT_OF_MEMORY;
    TxLockStatus s = txlock_open(name, &h->lock);
    if (s != TXLOCK_OK) { free(h); return map_status(s); }
    *lock = h;
    return STATUS_OK;
}

status_t semaphore_lock_timeout(transaction_lock_t *lock, uint32_t timeout_ms, int *abandoned)
{
    if (abandoned) *abandoned = 0;
    if (lock == NULL) return STATUS_INVALID_ARGUMENT;
    TxLockStatus s = txlock_acquire(&lock->lock, timeout_ms);
    if (s == TXLOCK_ABANDONED) {
        if (abandoned) *abandoned = 1;
        fprintf(stderr, "semaphore: previous lock owner died; shared data may need rollback\n");
    }
    return map_status(s);
}

status_t semaphore_lock(transaction_lock_t *lock)
{
    return semaphore_lock_timeout(lock, DEFAULT_LOCK_TIMEOUT_MS, NULL);
}

status_t semaphore_unlock(transaction_lock_t *lock)
{
    if (lock == NULL) return STATUS_INVALID_ARGUMENT;
    return txlock_release(&lock->lock) == TXLOCK_OK ? STATUS_OK : STATUS_SYNCHRONIZATION_ERROR;
}

void semaphore_close(transaction_lock_t *lock)
{
    if (lock == NULL) return;
    txlock_close(&lock->lock);
    free(lock);
}

status_t semaphore_remove(transaction_lock_t *lock)
{
    if (lock == NULL) return STATUS_INVALID_ARGUMENT;
>>>>>>> origin/main
    semaphore_close(lock);
    return STATUS_OK;
}
