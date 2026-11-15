#include "msg_queue.h"
#include <stdio.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/msg.h>


static key_t ipc_key(void) {
    key_t key = ftok(IPC_KEY_FILE, IPC_PROJ_ID);
    if (key == (key_t)-1) {
        perror("ftok failed for message queue");
    }
    return key;
}

int msg_queue_create(void) {
    key_t key = ipc_key();
    if (key == (key_t)-1) {
        return -1;
    }

    // Create mq with owner-only read/write permissions (0600)
    int qid = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (qid == -1) {
        perror("msgget (create) failed");
        return -1;
    }

    return qid;
}

int msg_queue_open(void) {
    key_t key = ipc_key();
    if (key == (key_t)-1) {
        return -1;
    }

    // Connect to an already created message queue
    int qid = msgget(key, 0600);
    if (qid == -1) {
        perror("failed to message queue");
        return -1;
    }

    return qid;
}

int msg_queue_send(int queue_id, const Message *msg) {
    if (msg == NULL) {
        errno = EINVAL;
        return -1;
    }

    // System V msgsnd expects the byte size excluding the leading 'long mtype'  as defined in the structure
    size_t payload_size = sizeof(Message) - sizeof(long);

    if (msgsnd(queue_id, msg, payload_size, 0) == -1) {
        perror("msgsnd failed");
        return -1;
    }

    return 0;
}

int msg_queue_receive(int queue_id, Message *msg, long msg_type) {
    if (msg == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t payload_size = sizeof(Message) - sizeof(long);

    /* Block until a message matching msg_type arrives */
    ssize_t bytes_read = msgrcv(queue_id, msg, payload_size, msg_type, 0);
    if (bytes_read == -1) {
        /* Do not treat signal interruption (e.g., SIGINT shutdown) as a fatal error log */
        if (errno != EINTR) {
            perror("msgrcv failed");
        }
        return -1;
    }

    return 0;
}

int msg_queue_remove(int queue_id) {
    if (msgctl(queue_id, IPC_RMID, NULL) == -1) {
        perror("msgctl (IPC_RMID) failed");
        return -1;
    }

    return 0;
}