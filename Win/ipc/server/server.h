#ifndef SERVER_H
#define SERVER_H

#include <windows.h>

#include "../common/protocol.h"
#include "../shared_memory/shared_memory.h"
#include "../synchronization/semaphore.h"

/* Manual-reset event every server process waits on. Set once to stop them all. */
#define SHUTDOWN_EVENT_NAME "Local\\BankShutdownEvent"
#define DATA_FILE           "bank.dat"
#define DEFAULT_WORKERS     8
#define MAX_WORKERS         32

/* What one server process needs to do its work. */
typedef struct {
    shared_memory_t *shm;
    bank_t *bank;
    transaction_lock_t *lock;
    HANDLE stop;
    int worker_id;      /* 0 in the parent */
    int use_lock;       /* 0 only with --nolock, to show a lost update */
} server_ctx_t;

/* State of the client currently connected to this worker. */
typedef struct {
    int logged_in;
    client_id_t client_id;
    session_id_t session_id;
} session_t;

/* client_registry.c */
void registry_make_id(client_id_t *id, unsigned index);
void registry_init(bank_t *bank);
account_t *registry_find(bank_t *bank, const client_id_t *id);
int registry_valid(const bank_t *bank);
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected);

/* authentication.c */
status_t auth_login(server_ctx_t *ctx, session_t *session, const login_request_t *request);
status_t auth_check(const session_t *session, const message_header_t *header);
void auth_logout(session_t *session);

/* transaction.c */
status_t transaction_execute(server_ctx_t *ctx, const transaction_request_t *request,
                             transaction_response_t *response);
status_t transaction_balance(server_ctx_t *ctx, const client_id_t *id, balance_response_t *response);

/* persistence.c */
status_t persistence_load(bank_t *bank);
status_t persistence_save(const bank_t *bank);

/* operator.c */
void operator_print_status(server_ctx_t *ctx);
void operator_start(server_ctx_t *ctx);

/* server.c */
int worker_run(server_ctx_t *ctx);

#endif /* SERVER_H */
