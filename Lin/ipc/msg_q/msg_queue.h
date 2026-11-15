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
