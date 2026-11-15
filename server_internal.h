#ifndef SERVER_INTERNAL_H
#define SERVER_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "../common/protocol.h"
#include "../common/status.h"
#include "../common/transaction.h"
#include "../common/types.h"
#include "../named_pipe/named_pipe.h"

/* ============================================================================
 * CrossIPC Windows Server Internal Coordination Header
 * Owner: @William (Win/ipc/server)
 * ============================================================================ */

typedef struct {
    bank_t           *bank;
    HANDLE            shm_handle;
    HANDLE            sem_handle;
    CRITICAL_SECTION  audit_cs;
    int               sync_enabled;
    money_t           initial_total;
    money_t           net_delta;
    uint64_t          total_transactions;
    uint64_t          failed_transactions;
    LARGE_INTEGER     qpc_freq;
    LARGE_INTEGER     first_tx_qpc;
    LARGE_INTEGER     last_tx_qpc;
    int               has_tx;
    volatile LONG     stop_requested;
} server_context_t;

/* client_registry.c */
void     registry_init(void);
void     registry_destroy(void);
status_t registry_register(const session_id_t *session_id, const client_id_t *client_id);
status_t registry_lookup(const session_id_t *session_id, client_id_t *out_client_id);
status_t registry_remove(const session_id_t *session_id);
uint32_t registry_active_count(void);

/* authentication.c */
status_t auth_handle_login(server_context_t *ctx,
                           const login_request_t *req,
                           session_id_t *out_session);
status_t auth_handle_logout(const session_id_t *session_id);
status_t auth_require_session(const session_id_t *session_id, client_id_t *out_client_id);

/* transaction.c */
status_t tx_handle_balance(server_context_t *ctx,
                           const balance_request_t *req,
                           balance_response_t *out_resp);
status_t tx_handle_operation(server_context_t *ctx,
                             const transaction_request_t *req,
                             transaction_response_t *out_resp);

/* persistence.c */
status_t persistence_init(server_context_t *ctx);
void     persistence_record_tx(server_context_t *ctx,
                               const transaction_request_t *req,
                               const transaction_response_t *resp,
                               money_t net_delta_change);
void     persistence_record_failure(server_context_t *ctx);
void     persistence_print_report(server_context_t *ctx);
void     persistence_close(void);

/* operator.c */
status_t operator_handle_status(server_context_t *ctx,
                                response_t *out_resp,
                                char *out_summary,
                                uint32_t max_len);
void     operator_print_banner(const server_context_t *ctx);

/* server.c */
status_t server_init(server_context_t *ctx, int sync_enabled);
status_t server_run(server_context_t *ctx);
void     server_shutdown(server_context_t *ctx);

#endif /* SERVER_INTERNAL_H */
