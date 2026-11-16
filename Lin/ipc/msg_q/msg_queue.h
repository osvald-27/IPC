 #ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <stdint.h>     /* Fixed-width integer types: uint32_t, int32_t, int64_t */
#include <sys/types.h>  /* System types needed by System V IPC headers */

#include "../common/types.h"
#include "../common/status.h"

/*
 * Both the client and server feed this same anchor file and single character
 * ('Q' for Queue) into ftok() so the kernel generates the exact same IPC key
 * without passing raw queue IDs over the command line. Using 'Q' keeps this key
 * distinct from shared memory ('S') and the semaphore ('M').
 */
#define DEFAULT_IPC_KEY_FILE "utilities/ipc.key"
#define MSG_PROJ_ID          'Q'

/*
 * Fixed character array lengths for strings inside the wire struct.
 * Notice CLIENT_ID_LEN is 8 (7 characters like "ACC0001" + 1 byte for '\0'),
 * and SESSION_ID_LEN / TXN_ID_LEN are 36 bytes so strings fit inline without
 * using pointers—raw pointers are meaningless across separate process spaces.
 */
#define CLIENT_ID_LEN        8
#define SESSION_ID_LEN       36
#define TXN_ID_LEN           36

/*
 * Reserved mtype for client-to-server requests; responses use the client's PID.
 * The 'L' suffix makes 1 a 'long' integer literal to match the 'long mtype'
 * field required by System V msgsnd() and msgrcv().
 */
#define MSG_REQUEST_TYPE     1L

/*
 * Operation codes are spaced by tens (1s for auth, 10s for queries, 20s for
 * money movements) so new commands can be added later without renumbering
 * existing ones, and raw integer codes are easy to spot in debug logs.
 */
typedef enum {
    MSG_NONE           = 0,
    MSG_ACCOUNT_CREATE = 1,
    MSG_LOGIN          = 2,
    MSG_LOGOUT         = 3,
    MSG_BALANCE        = 10,
    MSG_DEPOSIT        = 20,
    MSG_WITHDRAW       = 21,
    MSG_TRANSFER       = 22,
    MSG_STATUS_QUERY   = 30,
    MSG_SERVER_STATUS  = 40
} msg_operation_t;

/*
 * System V Message Queue structure.
 * Layout must match server.h byte-for-byte so msgsnd() and msgrcv() never
 * misalign fields across client and server processes.
 *
 * Syntax & layout notes:
 *   - 'long mtype' MUST be the very first member because the Linux kernel
 *     reads the first sizeof(long) bytes of the buffer to route messages.
 *   - Fixed-width types (uint32_t, int32_t, int64_t) guarantee identical byte
 *     sizes and struct padding across compilers instead of plain 'int'.
 *   - 'operation' and 'status_code' use int32_t rather than enum types directly
 *     because C compilers are allowed to pick different byte widths for enums.
 */
typedef struct {
    long     mtype;                       /* 1 for requests; client PID for responses */
    uint32_t request_id;                  /* Request identifier for idempotency */
    int32_t  sender_pid;                  /* Client process PID */
    char     client_id[CLIENT_ID_LEN];    /* Sender account ID, e.g. "ACC0001" */
    char     session_id[SESSION_ID_LEN];  /* Active session token */
    int32_t  operation;                   /* Operation code (msg_operation_t) */
    int64_t  amount;                      /* Amount in XAF */
    char     receiver_id[CLIENT_ID_LEN];  /* Target account ID for transfers */
    char     txn_id[TXN_ID_LEN];          /* Transaction ID assigned by server */
    int32_t  status_code;                 /* Server status code (status_t) */
} Message;

/*
 * Creates a brand-new System V message queue exclusively with owner-only
 * permissions (0600). 'int *queue_id' is an out-pointer where the function
 * writes the resulting kernel queue ID while returning a status_t code.
 */
status_t msg_queue_create(const char *key_path, char proj_id, int *queue_id);

/*
 * Attaches a client to an existing message queue created by the server without
 * IPC_CREAT, failing cleanly if the server isn't running yet.
 */
status_t msg_queue_open(const char *key_path, char proj_id, int *queue_id);

/*
 * Sends a Message onto the queue. 'const Message *msg' promises the function
 * will only read from your struct and never modify it. Automatically subtracts
 * sizeof(long) from sizeof(Message) as required by msgsnd().
 */
status_t msg_queue_send(int queue_id, const Message *msg);

/*
 * Blocks inside msgrcv() until a message matching 'msg_type' arrives and writes
 * it into the caller-supplied 'Message *msg' buffer.
 */
status_t msg_queue_receive(int queue_id, Message *msg, long msg_type);

/*
 * Destroys the message queue in the kernel via msgctl(..., IPC_RMID, NULL)
 * during server shutdown so IPC slots aren't leaked on the system.
 */
status_t msg_queue_remove(int queue_id);

#endif /* MSG_QUEUE_H */
