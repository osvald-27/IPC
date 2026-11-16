#ifndef WIN_PROTOCOL_H
#define WIN_PROTOCOL_H
#include <stdint.h>
#include "types.h"
#include "status.h"
#include "transaction.h"
#define ACCOUNT_COUNT 3U
#define INITIAL_BALANCE ((money_t)100000)
#define PIPE_NAME "\\\\.\\pipe\\CrossIPC_Bank"
#define SHM_NAME "Local\\CrossIPC_BankSharedMemory"
#define SEM_NAME "Local\\CrossIPC_BankMutex"
#define DATA_FILE "utilities/bank.dat"
#define DATA_FILE_TMP "utilities/bank.dat.tmp"
#define TRANSACTIONS_LOG "utilities/transactions.log"
#define DEFAULT_WORKERS 4
#define MAX_WORKERS 32
#define MAX_PENDING_DEPOSITS 64
#define IDEMPOTENCY_CACHE_SIZE 128
#define MAX_SESSIONS 64
#define CLIENT_ID_LEN 8
#define SESSION_ID_LEN 36
#define TXN_ID_LEN 36

typedef enum {
    MSG_NONE=0, MSG_ACCOUNT_CREATE=1, MSG_LOGIN=2, MSG_LOGOUT=3,
    MSG_BALANCE=10, MSG_DEPOSIT=20, MSG_WITHDRAW=21, MSG_TRANSFER=22,
    MSG_STATUS_QUERY=30, MSG_SERVER_STATUS=40
} msg_operation_t;

typedef struct {
    uint32_t request_id;
    uint32_t sender_pid;
    char client_id[CLIENT_ID_LEN];
    char session_id[SESSION_ID_LEN];
    int32_t operation;
    int64_t amount;
    char receiver_id[CLIENT_ID_LEN];
    char txn_id[TXN_ID_LEN];
    int32_t status_code;
} Message;
#endif
