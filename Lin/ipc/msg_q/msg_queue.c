/*
 * msg_queue.c
 *
 * Wraps Linux System V message queue syscalls (ftok, msgget, msgsnd, msgrcv, msgctl)
 * behind clean status_t return codes so the client and server don't have to deal with
 * raw kernel return values, errno checks, or mtype payload size math directly.
 */

#include "msg_queue.h"

#include <errno.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/msg.h>

/*
 * Turns the shared anchor file on disk plus a single project character ('Q') into a
 * reproducible System V key_t using ftok(). Both the server and every client process
 * run this same logic so they independently arrive at the exact same kernel queue key.
 */
static key_t derive_queue_key(const char *key_path, char proj_id) {
    /* Fall back to the default utilities/ipc.key path if the caller passes NULL */
    const char *path = (key_path != NULL) ? key_path : DEFAULT_IPC_KEY_FILE;
    key_t key = ftok(path, proj_id);

    /* If someone launches the client binary from inside client/ or msg_q/ instead of
     * the project root, try one directory up before giving up.
     */
    if (key == (key_t)-1 && key_path == NULL) {
        key = ftok("../utilities/ipc.key", proj_id);
    }

    if (key == (key_t)-1) {
        perror("ftok failed for message queue");
    }

    return key;
}

/*
 * Called once by the server at startup to allocate a fresh kernel message queue.
 */
status_t msg_queue_create(const char *key_path, char proj_id, int *queue_id) {
    if (queue_id == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    key_t key = derive_queue_key(key_path, proj_id);
    if (key == (key_t)-1) {
        return STATUS_QUEUE_ERROR;
    }

    /*
     * IPC_CREAT | IPC_EXCL forces msgget() to fail with EEXIST if an old queue
     * is still sitting in the kernel from an earlier crash, preventing the server
     * from accidentally processing stale leftover requests. Mode 0600 restricts
     * read/write access strictly to the user running the server.
     */
    int qid = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (qid == -1) {
        perror("msgget (create) failed");
        if (errno == EEXIST) {
            return STATUS_ALREADY_EXISTS;
        }
        if (errno == EACCES) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_QUEUE_ERROR;
    }

    *queue_id = qid;
    return STATUS_OK;
}

/*
 * Called by client processes to attach to the queue already created by the server.
 */
status_t msg_queue_open(const char *key_path, char proj_id, int *queue_id) {
    if (queue_id == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    key_t key = derive_queue_key(key_path, proj_id);
    if (key == (key_t)-1) {
        return STATUS_QUEUE_ERROR;
    }

    /*
     * Intentionally omit IPC_CREAT here: if the server isn't running yet, msgget()
     * fails with ENOENT so the client can report STATUS_NOT_FOUND instead of
     * silently creating an orphan queue with nobody listening on the other end.
     */
    int qid = msgget(key, 0600);
    if (qid == -1) {
        perror("msgget (open) failed");
        if (errno == ENOENT) {
            return STATUS_NOT_FOUND;
        }
        if (errno == EACCES) {
            return STATUS_PERMISSION_DENIED;
        }
        return STATUS_QUEUE_ERROR;
    }

    *queue_id = qid;
    return STATUS_OK;
}

/*
 * Pushes a Message struct onto the kernel queue.
 */
status_t msg_queue_send(int queue_id, const Message *msg) {
    /* System V requires msg->mtype > 0; 0 or negative values cause msgsnd() to fail */
    if (msg == NULL || msg->mtype <= 0) {
        return STATUS_INVALID_ARGUMENT;
    }

    /*
     * Crucial System V quirk: msgsnd() expects the size of the message *text/payload*
     * AFTER the leading 'long mtype' field, not sizeof(Message). Passing the full
     * sizeof(Message) would read 8 bytes past the end of the struct.
     */
    size_t payload_size = sizeof(Message) - sizeof(long);

    /* Flag 0 means block if the kernel queue buffer is temporarily full */
    if (msgsnd(queue_id, msg, payload_size, 0) == -1) {
        if (errno == EINTR) {
            return STATUS_INTERRUPTED;
        }
        perror("msgsnd failed");
        return STATUS_QUEUE_ERROR;
    }

    return STATUS_OK;
}

/*
 * Waits for and dequeues the next message matching msg_type:
 *   - Workers pass MSG_REQUEST_TYPE (1L) to pull the next incoming client request.
 *   - Clients pass their own PID (long)getpid() so they only receive replies meant for them.
 */
status_t msg_queue_receive(int queue_id, Message *msg, long msg_type) {
    if (msg == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* Exclude the leading 'long mtype' field just like msgsnd() */
    size_t payload_size = sizeof(Message) - sizeof(long);

    /* Block in the kernel until a matching message arrives (zero CPU polling) */
    ssize_t bytes_read = msgrcv(queue_id, msg, payload_size, msg_type, 0);
    if (bytes_read == -1) {
        /*
         * When Ctrl+C (SIGINT) or SIGTERM arrives during a blocking msgrcv(), the
         * kernel wakes the call with EINTR. Return STATUS_INTERRUPTED quietly so
         * callers can break out of their loop cleanly without printing noisy errors.
         */
        if (errno == EINTR) {
            return STATUS_INTERRUPTED;
        }
        perror("msgrcv failed");
        return STATUS_QUEUE_ERROR;
    }

    return STATUS_OK;
}

/*
 * Destroys the message queue in the kernel during server shutdown.
 * IPC_RMID takes effect immediately—any workers or clients still blocked inside
 * msgsnd/msgrcv wake up with EIDRM so they can exit instead of hanging forever.
 */
status_t msg_queue_remove(int queue_id) {
    if (msgctl(queue_id, IPC_RMID, NULL) == -1) {
        perror("msgctl (IPC_RMID) failed");
        return STATUS_QUEUE_ERROR;
    }

    return STATUS_OK;
}
