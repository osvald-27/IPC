#ifndef CLIENT_H
#define CLIENT_H

#include <windows.h>

#include "../common/protocol.h"
#include "../named_pipe/named_pipe.h"

#define CONNECT_TIMEOUT_MS 10000

/* One logged-in connection to the server. */
typedef struct {
    pipe_conn_t *conn;
    client_id_t client_id;
    session_id_t session_id;
    unsigned long next_request;
    HANDLE stop;                /* set by Ctrl+C */
} client_session_t;

/* session.c */
status_t session_open(client_session_t *s, const char *client_id, HANDLE stop);
status_t session_transaction(client_session_t *s, transaction_type_t type, const char *receiver_id,
                             money_t amount, transaction_response_t *response);
status_t session_balance(client_session_t *s, balance_response_t *response);
status_t session_server_check(client_session_t *s);
void session_close(client_session_t *s);

/* runner.c */
int client_interactive(const char *client_id, HANDLE stop);
int client_auto(const char *client_id, unsigned long count, unsigned seed, HANDLE stop);

#endif /* CLIENT_H */
