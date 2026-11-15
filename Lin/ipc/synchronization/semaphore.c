#include "semaphore.h"
#include <sys/sem.h>
#include <sys/ipc.h>
#include <stdio.h>

// Pour semctl, Linux demande cette structure
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

int semaphore_create(key_t key) {
    int semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    if (semid == -1) {
        perror("semget");
        return -1;
    }

    union semun arg;
    arg.val = 1; // 1 = libre
    if (semctl(semid, 0, SETVAL, arg) == -1) {
        perror("semctl SETVAL");
        return -1;
    }
    return semid;
}

int semaphore_lock(int semid) {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op = -1; // P : on prend
    op.sem_flg = 0; // on attend si occupé
    if (semop(semid, &op, 1) == -1) {
        perror("semop lock");
        return -1;
    }
    return 0;
}

int semaphore_unlock(int semid) {
    struct sembuf op;
    op.sem_num = 0;
    op.sem_op = 1; // V : on libère
    op.sem_flg = 0;
    if (semop(semid, &op, 1) == -1) {
        perror("semop unlock");
        return -1;
    }
    return 0;
}

int semaphore_remove(int semid) {
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl RMID");
        return -1;
    }
    return 0;
}
