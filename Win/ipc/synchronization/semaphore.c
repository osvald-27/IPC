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
    semaphore_close(lock);
    return STATUS_OK;
}
