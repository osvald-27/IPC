#ifndef SERVER_H
#define SERVER_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "../common/types.h"
#include "../common/status.h"
#include "../common/transaction.h"
#include "../common/protocol.h"
#include "../common/win_protocol.h"
#include "../synchronization/semaphore.h"

typedef struct {
    uint32_t request_id;
    int32_t client_pid;
    client_id_t client_id;
    money_t amount;
    char txn_id[MAX_TRANSACTION_ID + 1U];
    int active;
    uint64_t created_at;
} pending_deposit_t;

typedef struct {
    uint32_t request_id;
    client_id_t client_id;
    int32_t operation;
    status_t status;
    money_t final_balance;
    char txn_id[MAX_TRANSACTION_ID + 1U];
    uint64_t timestamp;
} idempotency_entry_t;

typedef struct {
    int active;
    client_id_t client_id;
    session_id_t session_id;
    int32_t client_pid;
    uint64_t login_time;
} session_entry_t;

typedef struct {
    account_t accounts[ACCOUNT_COUNT];
    pending_deposit_t pending[MAX_PENDING_DEPOSITS];
    int pending_count;
    idempotency_entry_t idempotency_cache[IDEMPOTENCY_CACHE_SIZE];
    int idempotency_idx;
    session_entry_t sessions[MAX_SESSIONS];
} bank_t;

/* The persisted bank_t must have the same 64-bit layout as Lin/ipc/server/server.h. */
#if defined(_WIN64) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(money_t) == 8, "money_t must remain 64-bit");
_Static_assert(sizeof(account_t) == 48, "account_t layout differs from Linux");
_Static_assert(sizeof(pending_deposit_t) == 80, "pending_deposit_t layout differs from Linux");
_Static_assert(sizeof(idempotency_entry_t) == 80, "idempotency_entry_t layout differs from Linux");
_Static_assert(sizeof(session_entry_t) == 64, "session_entry_t layout differs from Linux");
_Static_assert(sizeof(bank_t) == 19616, "bank_t layout differs from Linux; persistence is not compatible");
#endif

typedef struct {
    bank_t *bank;
    transaction_lock_t *lock;
    int worker_id;
    DWORD pid;
    int use_lock;
    int auto_approve;
} server_ctx_t;

typedef struct {
    int logged_in;
    client_id_t client_id;
    session_id_t session_id;
    uint32_t client_pid;
    uint64_t login_time;
} session_t;

void registry_make_id(client_id_t *id, unsigned index);
void registry_init(bank_t *bank);
account_t *registry_find(bank_t *bank, const client_id_t *id);
int registry_valid(const bank_t *bank);
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected);
status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, uint32_t client_pid);
status_t auth_check(server_ctx_t *ctx, const char *client_id_str, const char *session_id_str);
void auth_logout(server_ctx_t *ctx, const char *session_id_str);
status_t transaction_execute(server_ctx_t *ctx, const Message *request, Message *response);
status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response);
status_t transaction_status_query(server_ctx_t *ctx, uint32_t request_id, Message *response);
status_t transaction_approve_deposit(server_ctx_t *ctx, const char *txn_id);
status_t transaction_reject_deposit(server_ctx_t *ctx, const char *txn_id);
int transaction_get_pending(server_ctx_t *ctx, pending_deposit_t *list, int max_count);
status_t persistence_load(bank_t *bank);
status_t persistence_save(const bank_t *bank);
status_t persistence_log_transaction(const transaction_t *tx);
void operator_print_status(server_ctx_t *ctx);
void operator_start(server_ctx_t *ctx);
void operator_stop(void);
void server_handle_request(server_ctx_t *ctx, session_t *session, const Message *request, Message *response);
int server_run(server_ctx_t *ctx);
extern volatile LONG g_server_stop;

static inline const char *server_status_string(status_t s) {
    switch (s) {
    case STATUS_OK: return "OK"; case STATUS_ERROR: return "ERROR";
    case STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT"; case STATUS_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case STATUS_NOT_FOUND: return "NOT_FOUND"; case STATUS_ALREADY_EXISTS: return "ALREADY_EXISTS";
    case STATUS_PERMISSION_DENIED: return "PERMISSION_DENIED"; case STATUS_AUTH_FAILED: return "AUTH_FAILED";
    case STATUS_SESSION_INVALID: return "SESSION_INVALID"; case STATUS_SESSION_EXPIRED: return "SESSION_EXPIRED";
    case STATUS_ACCOUNT_NOT_FOUND: return "ACCOUNT_NOT_FOUND"; case STATUS_ACCOUNT_EXISTS: return "ACCOUNT_EXISTS";
    case STATUS_ACCOUNT_INACTIVE: return "ACCOUNT_INACTIVE"; case STATUS_INVALID_AMOUNT: return "INVALID_AMOUNT";
    case STATUS_INSUFFICIENT_FUNDS: return "INSUFFICIENT_FUNDS"; case STATUS_TRANSACTION_REJECTED: return "TRANSACTION_REJECTED";
    case STATUS_TRANSACTION_FAILED: return "TRANSACTION_FAILED"; case STATUS_TRANSACTION_PENDING: return "TRANSACTION_PENDING";
    case STATUS_DUPLICATE_REQUEST: return "DUPLICATE_REQUEST"; case STATUS_SELF_TRANSFER: return "SELF_TRANSFER";
    case STATUS_BALANCE_OVERFLOW: return "BALANCE_OVERFLOW"; case STATUS_QUEUE_ERROR: return "QUEUE_ERROR";
    case STATUS_QUEUE_FULL: return "QUEUE_FULL"; case STATUS_QUEUE_EMPTY: return "QUEUE_EMPTY";
    case STATUS_SHARED_MEMORY_ERROR: return "SHARED_MEMORY_ERROR"; case STATUS_SYNCHRONIZATION_ERROR: return "SYNCHRONIZATION_ERROR";
    case STATUS_TIMEOUT: return "TIMEOUT"; case STATUS_STORAGE_ERROR: return "STORAGE_ERROR";
    case STATUS_RECOVERY_ERROR: return "RECOVERY_ERROR"; case STATUS_SERVER_UNAVAILABLE: return "SERVER_UNAVAILABLE";
    case STATUS_SHUTTING_DOWN: return "SHUTTING_DOWN"; case STATUS_INTERRUPTED: return "INTERRUPTED";
    case STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR"; default: return "UNKNOWN";
    }
}
#endif
