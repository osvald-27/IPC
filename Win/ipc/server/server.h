#ifndef SERVER_H
#define SERVER_H

/* Avoid pulling in a bunch of legacy Windows headers we don't need,
 * which speeds up build times and prevents weird macro collisions.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include ../common/types.h
#include ../common/status.h
#include ../common/transaction.h
#include ../common/protocol.h
#include ../synchronization/semaphore.h

/* Banking system parameters */
/* Writing to a .tmp file first and renaming prevents corrupting bank.dat
 * if the server crashes or loses power mid-save.
 */
#define DATA_FILE            utilitiesank.dat
#define DATA_FILE_TMP        utilitiesank.dat.tmp
#define TRANSACTIONS_LOG     utilities	ransactions.log

#define DEFAULT_WORKERS      4
#define MAX_WORKERS          32
#define MAX_PENDING_DEPOSITS 64

#define CLIENT_ID_LEN        8
#define SESSION_ID_LEN       36
#define TXN_ID_LEN           36

/* Grouping operation codes into ranges (1s for auth, 10s for queries, 20s for money)
 * makes debugging raw wire packets much easier when reading logs.
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
 * Wire message structure sent over the Windows Named Pipe.
 * Mirrors the Linux message fields so client and server logic stay identical.
 * Fixed-width int types (uint32_t, int64_t) are used instead of plain int/long
 * so struct padding and byte sizes stay predictable across compilers.
 */
typedef struct {
    uint32_t request_id;                    /* Request identifier for idempotency */
    uint32_t sender_pid;                    /* Client process ID */
    char     client_id[CLIENT_ID_LEN];      /* Sender account ID, e.g. ACC0001 */
    char     session_id[SESSION_ID_LEN];    /* Active session token */
    int32_t  operation;                     /* Operation code (msg_operation_t) */
    int64_t  amount;                        /* Amount in XAF */
    char     receiver_id[CLIENT_ID_LEN];    /* Target account ID for transfers */
    char     txn_id[TXN_ID_LEN];            /* Transaction ID assigned by server */
    int32_t  status_code;                   /* Server status code (status_t) */
} Message;

/*
 * Deposits don't hit the balance right away unless auto-approve is on.
 * We hold them in a fixed-size queue so an operator can inspect and approve/reject
 * them without blocking other client requests on the pipe.
 */
typedef struct {
    uint32_t    request_id;
    uint32_t    client_pid;
    client_id_t client_id;
    money_t     amount;
    char        txn_id[MAX_TRANSACTION_ID + 1U];
    int         active;         /* 1 if this slot holds a waiting deposit, 0 if reusable */
    uint64_t    created_at;
} pending_deposit_t;

/*
 * Bundles everything a thread needs to run so we don't have to rely on
 * messy global variables when spawning multiple worker threads.
 */
typedef struct {
    HANDLE              shm_handle;
    bank_t             *bank;
    transaction_lock_t *lock;
    int                 worker_id;      /* 0 in main server, 1..N in worker threads */
    DWORD               pid;
    int                 use_lock;       /* 0 only with --nolock, to demonstrate a lost update */
    int                 auto_approve;   /* 1 to auto-approve deposits (for tests/benchmarks) */
    volatile LONG       stop;           /* volatile so worker loops don't cache the value in a register */
} server_ctx_t;

/*
 * Tracks who is currently authenticated on a given pipe connection.
 * Keeping this per-connection stops one client from spoofing another client's ID
 * once logged in.
 */
typedef struct {
    int          logged_in;
    client_id_t  client_id;
    session_id_t session_id;
    uint32_t     client_pid;
    uint64_t     login_time;
} session_t;

/* client_registry.c */
/* Manages accounts inside shared memory; consistency checks help prove whether
 * --nolock caused money to vanish or appear out of thin air during race tests.
 */
void registry_make_id(client_id_t *id, unsigned index);
void registry_init(bank_t *bank);
account_t *registry_find(bank_t *bank, const client_id_t *id);
int registry_valid(const bank_t *bank);
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected);

/* authentication.c */
status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, uint32_t client_pid);
status_t auth_check(const session_t *session, const char *client_id_str, const char *session_id_str);
void auth_logout(session_t *session);

/* transaction.c */
status_t transaction_execute(server_ctx_t *ctx, const Message *request, Message *response);
status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response);
status_t transaction_status_query(server_ctx_t *ctx, uint32_t request_id, Message *response);
status_t transaction_approve_deposit(server_ctx_t *ctx, const char *txn_id);
status_t transaction_reject_deposit(server_ctx_t *ctx, const char *txn_id);
int transaction_get_pending(pending_deposit_t *list, int max_count);

/* persistence.c */
status_t persistence_load(bank_t *bank);
status_t persistence_save(const bank_t *bank);
status_t persistence_log_transaction(const transaction_t *tx);

/* operator.c */
/* Runs on its own thread so the admin can type commands without freezing pipe I/O */
void operator_print_status(server_ctx_t *ctx);
void operator_start(server_ctx_t *ctx);
void operator_stop(void);

/* server.c */
void server_handle_request(server_ctx_t *ctx, session_t *session, const Message *request, Message *response);
int server_run(server_ctx_t *ctx);

/*
 * Marked static inline so any .c file that includes server.h can turn status enums
 * into readable log strings without causing multiple-definition linker errors.
 */
static inline const char *server_status_string(status_t s) {
    switch (s) {
        case STATUS_OK:                    return OK;
        case STATUS_ERROR:                 return ERROR;
        case STATUS_INVALID_ARGUMENT:      return INVALID_ARGUMENT;
        case STATUS_OUT_OF_MEMORY:         return OUT_OF_MEMORY;
        case STATUS_NOT_FOUND:             return NOT_FOUND;
        case STATUS_ALREADY_EXISTS:        return ALREADY_EXISTS;
        case STATUS_PERMISSION_DENIED:     return PERMISSION_DENIED;
        case STATUS_AUTH_FAILED:           return AUTH_FAILED;
        case STATUS_SESSION_INVALID:       return SESSION_INVALID;
        case STATUS_SESSION_EXPIRED:       return SESSION_EXPIRED;
        case STATUS_ACCOUNT_NOT_FOUND:     return ACCOUNT_NOT_FOUND;
        case STATUS_ACCOUNT_EXISTS:        return ACCOUNT_EXISTS;
        case STATUS_ACCOUNT_INACTIVE:      return ACCOUNT_INACTIVE;
        case STATUS_INVALID_AMOUNT:        return INVALID_AMOUNT;
        case STATUS_INSUFFICIENT_FUNDS:    return INSUFFICIENT_FUNDS;
        case STATUS_TRANSACTION_REJECTED:  return TRANSACTION_REJECTED;
        case STATUS_TRANSACTION_FAILED:    return TRANSACTION_FAILED;
        case STATUS_TRANSACTION_PENDING:   return TRANSACTION_PENDING;
        case STATUS_DUPLICATE_REQUEST:     return DUPLICATE_REQUEST;
        case STATUS_SELF_TRANSFER:         return SELF_TRANSFER;
        case STATUS_BALANCE_OVERFLOW:      return BALANCE_OVERFLOW;
        case STATUS_QUEUE_ERROR:           return QUEUE_ERROR;
        case STATUS_QUEUE_FULL:            return QUEUE_FULL;
        case STATUS_QUEUE_EMPTY:           return QUEUE_EMPTY;
        case STATUS_SHARED_MEMORY_ERROR:   return SHARED_MEMORY_ERROR;
        case STATUS_SYNCHRONIZATION_ERROR: return SYNCHRONIZATION_ERROR;
        case STATUS_TIMEOUT:               return TIMEOUT;
        case STATUS_STORAGE_ERROR:         return STORAGE_ERROR;
        case STATUS_RECOVERY_ERROR:        return RECOVERY_ERROR;
        case STATUS_SERVER_UNAVAILABLE:    return SERVER_UNAVAILABLE;
        case STATUS_SHUTTING_DOWN:         return SHUTTING_DOWN;
        case STATUS_INTERRUPTED:           return INTERRUPTED;
        case STATUS_INTERNAL_ERROR:        return INTERNAL_ERROR;
    }
    return UNKNOWN;
}

#endif /* SERVER_H */

