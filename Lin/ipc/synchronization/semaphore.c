#define _POSIX_C_SOURCE 200809L

#include "semaphore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/types.h>

/*
 * Internal Linux representation kept private to this file.
 * Windows can define this same struct name around a HANDLE instead.
 */
struct transaction_lock {
    int sem_id;
};

/*
 * Linux leaves union semun undefined in <sys/sem.h>, so we declare it
 * manually to pass our starting value into semctl().
 */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

status_t semaphore_create(const char *key_path, char proj_id, transaction_lock_t **lock) {
    if (key_path == NULL || lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* Derive a consistent IPC key from the shared key file on disk */
    key_t key = ftok(key_path, proj_id);
    if (key == -1) {
        perror("ftok semaphore");
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    /*
     * IPC_CREAT | IPC_EXCL forces creation to fail if a leftover semaphore
     * already exists from an earlier crash, so we never attach to stale state.
     * 0600 restricts access strictly to the server user.
     */
    int id = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    if (id == -1) {
        perror("semget create");
        if (errno == EEXIST) {
            return STATUS_ALREADY_EXISTS;
        }
        if (errno == EACCES) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    /*
     * Start at 1 so the semaphore behaves like a mutex: one worker can
     * decrement it to 0 and proceed while others wait until it returns to 1.
     */
    union semun arg;
    arg.val = 1;

    if (semctl(id, 0, SETVAL, arg) == -1) {
        perror("semctl SETVAL");
        semctl(id, 0, IPC_RMID);
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    transaction_lock_t *handle = malloc(sizeof(transaction_lock_t));
    if (handle == NULL) {
        semctl(id, 0, IPC_RMID);
        return STATUS_OUT_OF_MEMORY;
    }

    handle->sem_id = id;
    *lock = handle;
    return STATUS_OK;
}

status_t semaphore_open(const char *key_path, char proj_id, transaction_lock_t **lock) {
    if (key_path == NULL || lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    key_t key = ftok(key_path, proj_id);
    if (key == -1) {
        perror("ftok semaphore open");
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    /* Attach to the existing semaphore without creating a new one */
    int id = semget(key, 1, 0600);
    if (id == -1) {
        perror("semget open");
        if (errno == ENOENT) {
            return STATUS_NOT_FOUND;
        }
        if (errno == EACCES) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    transaction_lock_t *handle = malloc(sizeof(transaction_lock_t));
    if (handle == NULL) {
        return STATUS_OUT_OF_MEMORY;
    }

    handle->sem_id = id;
    *lock = handle;
    return STATUS_OK;
}

status_t semaphore_lock(transaction_lock_t *lock) {
    if (lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /*
     * SEM_UNDO tells the kernel to automatically reverse our -1 operation
     * if a worker crashes while holding the lock, so other workers never
     * get stuck waiting forever.
     */
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = -1;
    op.sem_flg = SEM_UNDO;

    if (semop(lock->sem_id, &op, 1) == -1) {
        if (errno == EINTR) {
            return STATUS_INTERRUPTED;
        }
        perror("semop lock");
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    return STATUS_OK;
}

status_t semaphore_unlock(transaction_lock_t *lock) {
    if (lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    struct sembuf op;
    op.sem_num = 0;
    op.sem_op  = 1;
    op.sem_flg = SEM_UNDO;

    if (semop(lock->sem_id, &op, 1) == -1) {
        perror("semop unlock");
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    return STATUS_OK;
}

void semaphore_close(transaction_lock_t *lock) {
    free(lock);
}

status_t semaphore_remove(transaction_lock_t *lock) {
    if (lock == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /*
     * IPC_RMID deletes the semaphore from the kernel right away and wakes
     * any waiting workers so they can exit cleanly on shutdown.
     */
    if (semctl(lock->sem_id, 0, IPC_RMID) == -1) {
        perror("semctl IPC_RMID");
        free(lock);
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    free(lock);
    return STATUS_OK;
}
