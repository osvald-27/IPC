#ifndef SERVER_H
#define SERVER_H

/* Expose POSIX.1-2008 APIs (like nanosleep, sigaction, etc.) cleanly under strict C flags */
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>

#include "../common/types.h"
#include "../common/status.h"
#include "../common/transaction.h"
#include "../common/protocol.h"
#include "../synchronization/semaphore.h"

/* Banking system parameters */
#define ACCOUNT_COUNT          3U
#define INITIAL_BALANCE        ((money_t)100000)   /* 100,000 XAF per account */

#define IPC_KEY_FILE           "utilities/ipc.key"
#define DEFAULT_IPC_KEY_FILE   "utilities/ipc.key"
#define SHM_PROJ_ID            'S'
#define SEM_PROJ_ID            'M'
#define MSG_PROJ_ID            'Q'

#define DATA_FILE              "utilities/bank.dat"
#define DATA_FILE_TMP          "utilities/bank.dat.tmp"
#define TRANSACTIONS_LOG       "utilities/transactions.log"

#define DEFAULT_WORKERS        4
#define MAX_WORKERS            32
#define MAX_PENDING_DEPOSITS   64
#define IDEMPOTENCY_CACHE_SIZE 128
#define MAX_SESSIONS           64

#define CLIENT_ID_LEN          8
#define SESSION_ID_LEN         36
#define TXN_ID_LEN             36

#define MSG_REQUEST_TYPE       1L

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

/* Pending deposit record awaiting operator approval */
typedef struct {
    uint32_t    request_id;
    int32_t     client_pid;
    client_id_t client_id;
    money_t     amount;
    char        txn_id[MAX_TRANSACTION_ID + 1U];
    int         active;
    uint64_t    created_at;
} pending_deposit_t;

/* Cache entry for request idempotency across all forked workers */
typedef struct {
    uint32_t    request_id;
    client_id_t client_id;
    int32_t     operation;
    status_t    status;
    money_t     final_balance;
    char        txn_id[MAX_TRANSACTION_ID + 1U];
    uint64_t    timestamp;
} idempotency_entry_t;

/* Active client session stored in shared memory so all forked workers share it */
typedef struct {
    int          active;
    client_id_t  client_id;
    session_id_t session_id;
    int32_t      client_pid;
    uint64_t     login_time;
} session_entry_t;

/* Authoritative Bank state in shared memory */
typedef struct {
    account_t           accounts[ACCOUNT_COUNT];
    pending_deposit_t   pending[MAX_PENDING_DEPOSITS];
    int                 pending_count;
    idempotency_entry_t idempotency_cache[IDEMPOTENCY_CACHE_SIZE];
    int                 idempotency_idx;
    session_entry_t     sessions[MAX_SESSIONS];
} bank_t;

typedef struct {
    int                    shm_id;
    bank_t                *bank;
    transaction_lock_t    *lock;
    int                    msg_qid;
    int                    worker_id;
    pid_t                  pid;
    int                    use_lock;
    int                    auto_approve;
    volatile sig_atomic_t  stop;
} server_ctx_t;

typedef struct {
    int          logged_in;
    client_id_t  client_id;
    session_id_t session_id;
    pid_t        client_pid;
    uint64_t     login_time;
} session_t;

/* client_registry.c */
void registry_make_id(client_id_t *id, unsigned index);
void registry_init(bank_t *bank);
account_t *registry_find(bank_t *bank, const client_id_t *id);
int registry_valid(const bank_t *bank);
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected);

/* authentication.c */
status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, pid_t client_pid);
status_t auth_check(server_ctx_t *ctx, const char *client_id_str, const char *session_id_str);
void auth_logout(server_ctx_t *ctx, const char *session_id_str);

/* transaction.c */
status_t transaction_execute(server_ctx_t *ctx, const Message *request, Message *response);
status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response);
status_t transaction_status_query(server_ctx_t *ctx, uint32_t request_id, Message *response);
status_t transaction_approve_deposit(server_ctx_t *ctx, const char *txn_id);
status_t transaction_reject_deposit(server_ctx_t *ctx, const char *txn_id);
int transaction_get_pending(server_ctx_t *ctx, pending_deposit_t *list, int max_count);

/* persistence.c */
status_t persistence_load(bank_t *bank);
status_t persistence_save(const bank_t *bank);
status_t persistence_log_transaction(const transaction_t *tx);

/* operator.c */
void operator_print_status(server_ctx_t *ctx);
void operator_start(server_ctx_t *ctx);
void operator_stop(void);

/* server.c */
int worker_run(server_ctx_t *ctx);
int server_msg_send(int qid, const Message *msg);
int server_msg_recv(int qid, Message *msg, long mtype);

static inline const char *server_status_string(status_t s) {
    switch (s) {
    case STATUS_OK:                    return "OK";
    case STATUS_ERROR:                 return "ERROR";
    case STATUS_INVALID_ARGUMENT:      return "INVALID_ARGUMENT";
    case STATUS_OUT_OF_MEMORY:         return "OUT_OF_MEMORY";
    case STATUS_NOT_FOUND:             return "NOT_FOUND";
    case STATUS_ALREADY_EXISTS:        return "ALREADY_EXISTS";
    case STATUS_PERMISSION_DENIED:     return "PERMISSION_DENIED";
    case STATUS_AUTH_FAILED:           return "AUTH_FAILED";
    case STATUS_SESSION_INVALID:       return "SESSION_INVALID";
    case STATUS_SESSION_EXPIRED:       return "SESSION_EXPIRED";
    case STATUS_ACCOUNT_NOT_FOUND:     return "ACCOUNT_NOT_FOUND";
    case STATUS_ACCOUNT_EXISTS:        return "ACCOUNT_EXISTS";
    case STATUS_ACCOUNT_INACTIVE:      return "ACCOUNT_INACTIVE";
    case STATUS_INVALID_AMOUNT:        return "INVALID_AMOUNT";
    case STATUS_INSUFFICIENT_FUNDS:    return "INSUFFICIENT_FUNDS";
    case STATUS_TRANSACTION_REJECTED:  return "TRANSACTION_REJECTED";
    case STATUS_TRANSACTION_FAILED:    return "TRANSACTION_FAILED";
    case STATUS_TRANSACTION_PENDING:   return "TRANSACTION_PENDING";
    case STATUS_DUPLICATE_REQUEST:     return "DUPLICATE_REQUEST";
    case STATUS_SELF_TRANSFER:         return "SELF_TRANSFER";
    case STATUS_BALANCE_OVERFLOW:      return "BALANCE_OVERFLOW";
    case STATUS_QUEUE_ERROR:           return "QUEUE_ERROR";
    case STATUS_QUEUE_FULL:            return "QUEUE_FULL";
    case STATUS_QUEUE_EMPTY:           return "QUEUE_EMPTY";
    case STATUS_SHARED_MEMORY_ERROR:   return "SHARED_MEMORY_ERROR";
    case STATUS_SYNCHRONIZATION_ERROR: return "SYNCHRONIZATION_ERROR";
    case STATUS_TIMEOUT:               return "TIMEOUT";
    case STATUS_STORAGE_ERROR:         return "STORAGE_ERROR";
    case STATUS_RECOVERY_ERROR:        return "RECOVERY_ERROR";
    case STATUS_SERVER_UNAVAILABLE:    return "SERVER_UNAVAILABLE";
    case STATUS_SHUTTING_DOWN:         return "SHUTTING_DOWN";
    case STATUS_INTERRUPTED:           return "INTERRUPTED";
    case STATUS_INTERNAL_ERROR:        return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

#endif /* SERVER_H */
