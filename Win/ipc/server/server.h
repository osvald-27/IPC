#ifndef SERVER_H
#define SERVER_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../common/types.h"
#include "../common/status.h"
#include "../common/transaction.h"
#include "../common/protocol.h"
#include "../synchronization/semaphore.h"
#define DATA_FILE "utilities/bank.dat"
#define DATA_FILE_TMP "utilities/bank.dat.tmp"
#define TRANSACTIONS_LOG "utilities/transactions.log"
#define DEFAULT_WORKERS 4
#define MAX_WORKERS 32
#define MAX_PENDING_DEPOSITS 64
#define CLIENT_ID_LEN 8
#define SESSION_ID_LEN 37
#define TXN_ID_LEN 37
#define IDEMPOTENCY_CACHE_SIZE 256
#define REQUEST_TIMEOUT_MS 10000
typedef enum { MSG_NONE=0, MSG_ACCOUNT_CREATE=1, MSG_LOGIN=2, MSG_LOGOUT=3, MSG_BALANCE=10, MSG_DEPOSIT=20, MSG_WITHDRAW=21, MSG_TRANSFER=22, MSG_STATUS_QUERY=30, MSG_SERVER_STATUS=40 } msg_operation_t;
typedef struct { uint32_t magic; uint16_t version; uint16_t header_size; uint32_t request_id; uint32_t sender_pid; uint32_t operation; uint32_t payload_size; char client_id[CLIENT_ID_LEN]; char session_id[SESSION_ID_LEN]; int64_t amount; char receiver_id[CLIENT_ID_LEN]; char txn_id[TXN_ID_LEN]; int32_t status_code; } Message;
#define MESSAGE_MAGIC 0x43524950U
#define MESSAGE_VERSION 1U
typedef struct { uint32_t request_id; uint32_t client_pid; client_id_t client_id; money_t amount; char txn_id[MAX_TRANSACTION_ID+1U]; int active; uint64_t created_at; } pending_deposit_t;
typedef struct { uint32_t request_id; client_id_t client_id; int32_t operation; status_t status; money_t final_balance; char txn_id[MAX_TRANSACTION_ID+1U]; uint64_t timestamp; } idempotency_entry_t;
typedef struct { HANDLE shm_handle; bank_t *bank; transaction_lock_t *lock; int worker_id; DWORD pid; int use_lock; int auto_approve; volatile LONG stop; } server_ctx_t;
typedef struct { int logged_in; client_id_t client_id; session_id_t session_id; uint32_t client_pid; uint64_t login_time; } session_t;
void registry_make_id(client_id_t *id,unsigned index);
void registry_init(bank_t *bank);
account_t *registry_find(bank_t *bank,const client_id_t *id);
int registry_valid(const bank_t *bank);
int registry_consistent(const bank_t *bank,money_t *actual,money_t *expected);
status_t auth_login(server_ctx_t *ctx,session_t *session,const char *client_id_str,uint32_t client_pid);
status_t auth_check(const session_t *session,const char *client_id_str,const char *session_id_str);
void auth_logout(session_t *session);
status_t transaction_execute(server_ctx_t *ctx,const Message *request,Message *response);
status_t transaction_balance(server_ctx_t *ctx,const client_id_t *id,balance_response_t *response);
status_t transaction_status_query(server_ctx_t *ctx,uint32_t request_id,Message *response);
status_t transaction_approve_deposit(server_ctx_t *ctx,const char *txn_id);
status_t transaction_reject_deposit(server_ctx_t *ctx,const char *txn_id);
int transaction_get_pending(pending_deposit_t *list,int max_count);
status_t persistence_load(bank_t *bank);
status_t persistence_save(const bank_t *bank);
status_t persistence_log_transaction(const transaction_t *transaction);
status_t semaphore_create(const char *name,transaction_lock_t **lock);
status_t semaphore_open(const char *name,transaction_lock_t **lock);
status_t semaphore_lock(transaction_lock_t *lock);
status_t semaphore_unlock(transaction_lock_t *lock);
void semaphore_remove(transaction_lock_t *lock);
status_t shared_memory_create(const char *name,HANDLE *handle,bank_t **bank);
status_t shared_memory_open(const char *name,HANDLE *handle,bank_t **bank);
void shared_memory_close(HANDLE handle,bank_t *bank);
void server_handle_request(server_ctx_t *ctx,session_t *session,const Message *request,Message *response);
int server_run(server_ctx_t *ctx);
void operator_print_status(server_ctx_t *ctx);
void operator_start(server_ctx_t *ctx);
void operator_stop(void);
static inline const char *server_status_string(status_t s) {
switch(s) {
case STATUS_OK:return "OK";
case STATUS_ERROR:return "ERROR";
case STATUS_INVALID_ARGUMENT:return "INVALID_ARGUMENT";
case STATUS_OUT_OF_MEMORY:return "OUT_OF_MEMORY";
case STATUS_NOT_FOUND:return "NOT_FOUND";
case STATUS_ALREADY_EXISTS:return "ALREADY_EXISTS";
case STATUS_PERMISSION_DENIED:return "PERMISSION_DENIED";
case STATUS_AUTH_FAILED:return "AUTH_FAILED";
case STATUS_SESSION_INVALID:return "SESSION_INVALID";
case STATUS_SESSION_EXPIRED:return "SESSION_EXPIRED";
case STATUS_ACCOUNT_NOT_FOUND:return "ACCOUNT_NOT_FOUND";
case STATUS_ACCOUNT_EXISTS:return "ACCOUNT_EXISTS";
case STATUS_ACCOUNT_INACTIVE:return "ACCOUNT_INACTIVE";
case STATUS_INVALID_AMOUNT:return "INVALID_AMOUNT";
case STATUS_INSUFFICIENT_FUNDS:return "INSUFFICIENT_FUNDS";
case STATUS_TRANSACTION_REJECTED:return "TRANSACTION_REJECTED";
case STATUS_TRANSACTION_FAILED:return "TRANSACTION_FAILED";
case STATUS_TRANSACTION_PENDING:return "TRANSACTION_PENDING";
case STATUS_DUPLICATE_REQUEST:return "DUPLICATE_REQUEST";
case STATUS_SELF_TRANSFER:return "SELF_TRANSFER";
case STATUS_BALANCE_OVERFLOW:return "BALANCE_OVERFLOW";
case STATUS_QUEUE_ERROR:return "QUEUE_ERROR";
case STATUS_QUEUE_FULL:return "QUEUE_FULL";
case STATUS_QUEUE_EMPTY:return "QUEUE_EMPTY";
case STATUS_SHARED_MEMORY_ERROR:return "SHARED_MEMORY_ERROR";
case STATUS_SYNCHRONIZATION_ERROR:return "SYNCHRONIZATION_ERROR";
case STATUS_TIMEOUT:return "TIMEOUT";
case STATUS_STORAGE_ERROR:return "STORAGE_ERROR";
case STATUS_RECOVERY_ERROR:return "RECOVERY_ERROR";
case STATUS_SERVER_UNAVAILABLE:return "SERVER_UNAVAILABLE";
case STATUS_SHUTTING_DOWN:return "SHUTTING_DOWN";
case STATUS_INTERRUPTED:return "INTERRUPTED";
case STATUS_INTERNAL_ERROR:return "INTERNAL_ERROR";
default:return "UNKNOWN";
}
}
#endif
