#include <stdint.h>
#include <sys/types.h>

//Path and project ID used by ftok() for ipc
#define IPC_KEY_FILE "../utilities/ipc"
#define IPC_PROJ_ID  'P'


#define CLIENT_ID_LEN   8
#define SESSION_ID_LEN  32
#define MSG_REQUEST_TYPE 1L  // Reserved mtype for client-to-server requests

typedef enum {
    MSG_ACCOUNT_CREATE = 1,
    MSG_LOGIN,
    MSG_BALANCE,
    MSG_DEPOSIT,
    MSG_WITHDRAW,
    MSG_TRANSFER,
    MSG_STATUS_QUERY,
    MSG_LOGOUT
} MessageType;

// System V Message Structure
typedef struct {
    long mtype;                          
    uint32_t request_id;                  
    int32_t sender_pid;                   
    char client_id[CLIENT_ID_LEN];        
    char session_id[SESSION_ID_LEN];
    int32_t operation;                    
    int64_t amount;                      
    char receiver_id[CLIENT_ID_LEN]; 
    int32_t status_code;                  
} Message;



//Creates a new System V message queue 
int msg_queue_create(void);

//Opens an existing System V message queue 
int msg_queue_open(void);

//Sends a Message structure to the queue
int msg_queue_send(int queue_id, const Message *msg);

// Receives a Message matching msg_type
int msg_queue_receive(int queue_id, Message *msg, long msg_type);

// Destroy the message queue
int msg_queue_remove(int queue_id);
#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include <stdint.h>
#include <sys/types.h>

#include "../common/types.h"
#include "../common/status.h"

/* Must match server.h so client and server derive the exact same System V IPC key */
#define DEFAULT_IPC_KEY_FILE "utilities/ipc.key"
#define MSG_PROJ_ID          'Q'

#define CLIENT_ID_LEN        8
#define SESSION_ID_LEN       36
#define TXN_ID_LEN           36

/* Reserved mtype for client-to-server requests; responses use the client's PID */
#define MSG_REQUEST_TYPE     1L

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
 * Layout must match server.h byte-for-byte so msgsnd() and msgrcv()
 * never misalign fields across client and server processes.
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

/* Creates a new System V message queue exclusively with owner-only permissions (0600) */
status_t msg_queue_create(const char *key_path, char proj_id, int *queue_id);

/* Attaches to an existing System V message queue created by the server */
status_t msg_queue_open(const char *key_path, char proj_id, int *queue_id);

/* Sends a Message structure to the queue (automatically excludes leading mtype from size) */
status_t msg_queue_send(int queue_id, const Message *msg);

/* Blocks until a Message matching msg_type arrives on the queue */
status_t msg_queue_receive(int queue_id, Message *msg, long msg_type);

/* Removes the message queue from the kernel during server cleanup */
status_t msg_queue_remove(int queue_id);

#endif /* MSG_QUEUE_H */
