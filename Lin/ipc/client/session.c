/*
 * session.c (Linux Client Session & System V Message Queue Adapter)
 *
 * Owns the client's active session state, assigns monotonically increasing
 * request IDs for server-side idempotency, and handles message-queue
 * round-trips (sending with mtype = 1 and waiting on mtype = client PID).
 *
 * Cross-platform note:
 * Because cli.c and main.c only call the public session_*() functions below
 * and never touch queue_id or msg_queue_*() directly, porting this file to
 * Win/ipc/client/session.c only changes the internal transport inside
 * session_connect(), session_disconnect(), and send_and_receive() to use a
 * Windows Named Pipe handle.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../common/types.h"
#include "../common/status.h"
#include "../msg_q/msg_queue.h"

/*
 * Keeps all per-client transport and session state private to session.c so
 * cli.c and main.c never touch raw System V queue IDs or PID routing directly.
 * Notice how three distinct concepts stay separate:
 *   - client_id  : Logical bank account (e.g. "ACC0001")
 *   - pid        : Running OS process ID used for message-queue reply routing
 *   - session_id : Server-issued token identifying this authenticated login
 */
typedef struct {
    int      queue_id;
    pid_t    pid;
    int      logged_in;
    char     client_id[CLIENT_ID_LEN];
    char     session_id[SESSION_ID_LEN];
    uint32_t next_request_id;
    uint32_t last_request_id;
    char     last_txn_id[TXN_ID_LEN];
} client_session_t;

static client_session_t g_session;

/*
 * Populates the common wire fields on an outgoing request and advances the
 * monotonic request_id counter. Saving last_request_id before sending lets
 * main.c poll the server for the true outcome if Ctrl+C interrupts mid-call.
 */
static void prepare_request(Message *req, msg_operation_t op) {
    memset(req, 0, sizeof(*req));

    /* All clients send requests with mtype = 1 so any idle server worker picks them up */
    req->mtype      = MSG_REQUEST_TYPE;
    req->request_id = g_session.next_request_id++;
    req->sender_pid = (int32_t)g_session.pid;
    req->operation  = (int32_t)op;

    /* Leave 1 byte for '\0' so wire strings are always safely terminated */
    strncpy(req->client_id, g_session.client_id, CLIENT_ID_LEN - 1);
    req->client_id[CLIENT_ID_LEN - 1] = '\0';

    strncpy(req->session_id, g_session.session_id, SESSION_ID_LEN - 1);
    req->session_id[SESSION_ID_LEN - 1] = '\0';

    g_session.last_request_id = req->request_id;
}

/*
 * Sends one request onto the queue and blocks until the matching reply arrives.
 *
 * Why we check resp->request_id in a loop:
 * Linux reuses PIDs after processes exit. If a previous client with our same
 * PID exited before reading its reply, a stale message could still be sitting
 * in the kernel queue. Matching request_id guarantees we discard stale replies
 * and only accept the response to our own request.
 */
static status_t send_and_receive(const Message *req, Message *resp) {
    if (req == NULL || resp == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    status_t st = msg_queue_send(g_session.queue_id, req);
    if (st != STATUS_OK) {
        return st;
    }

    while (1) {
        memset(resp, 0, sizeof(*resp));

        /* Filter strictly on mtype = (long)g_session.pid so we never steal another client's reply */
        st = msg_queue_receive(g_session.queue_id, resp, (long)g_session.pid);
        if (st != STATUS_OK) {
            return st;
        }

        if (resp->request_id == req->request_id) {
            break;
        }
    }

    /* Cache the latest server-assigned transaction ID so 'status' with no args can query it */
    if (resp->txn_id[0] != '\0') {
        strncpy(g_session.last_txn_id, resp->txn_id, TXN_ID_LEN - 1);
        g_session.last_txn_id[TXN_ID_LEN - 1] = '\0';
    }

    return (status_t)resp->status_code;
}

/*
 * Opens the server's existing System V message queue and initializes session
 * state. We start next_request_id at 1 because the server treats request_id == 0
 * as an uninitialized packet and skips idempotency caching.
 */
status_t session_connect(const char *key_path) {
    memset(&g_session, 0, sizeof(g_session));
    g_session.queue_id        = -1;
    g_session.pid             = getpid();
    g_session.next_request_id = 1;

    status_t st = msg_queue_open(key_path, MSG_PROJ_ID, &g_session.queue_id);
    if (st == STATUS_NOT_FOUND) {
        /* Translate raw ENOENT into a clear message that the server isn't running */
        return STATUS_SERVER_UNAVAILABLE;
    }

    return st;
}

/*
 * Clears local session state on exit.
 * Notice we never call msg_queue_remove() from a client—the server owns the
 * kernel message queue, so deleting it here would cut off all other clients.
 */
void session_disconnect(void) {
    memset(&g_session, 0, sizeof(g_session));
    g_session.queue_id = -1;
}

/* Small read-only accessors so cli.c and main.c can inspect state without touching g_session */
int session_is_logged_in(void) {
    return g_session.logged_in;
}

const char *session_client_id(void) {
    return g_session.client_id;
}

uint32_t session_last_request_id(void) {
    return g_session.last_request_id;
}

const char *session_last_txn_id(void) {
    return g_session.last_txn_id;
}

/*
 * Authenticates the given account ID with the server and saves the returned
 * session token for all subsequent commands. Wipes client_id on failure so
 * stale input doesn't linger in memory.
 */
status_t session_login(const char *client_id_str) {
    if (client_id_str == NULL || client_id_str[0] == '\0') {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(g_session.client_id, 0, sizeof(g_session.client_id));
    strncpy(g_session.client_id, client_id_str, CLIENT_ID_LENGTH);
    g_session.client_id[CLIENT_ID_LENGTH] = '\0';

    Message req;
    Message resp;
    prepare_request(&req, MSG_LOGIN);

    status_t st = send_and_receive(&req, &resp);
    if (st == STATUS_OK) {
        g_session.logged_in = 1;
        strncpy(g_session.session_id, resp.session_id, SESSION_ID_LEN - 1);
        g_session.session_id[SESSION_ID_LEN - 1] = '\0';
    } else {
        memset(g_session.client_id, 0, sizeof(g_session.client_id));
        g_session.logged_in = 0;
    }

    return st;
}

/*
 * Notifies the server to invalidate our session token immediately, and clears
 * local session fields even if the server didn't respond (e.g. during shutdown).
 */
status_t session_logout(void) {
    if (!g_session.logged_in) {
        return STATUS_OK;
    }

    Message req;
    Message resp;
    prepare_request(&req, MSG_LOGOUT);

    status_t st = send_and_receive(&req, &resp);
    g_session.logged_in = 0;
    memset(g_session.session_id, 0, sizeof(g_session.session_id));
    return st;
}

/* Queries the authoritative account balance directly from the server */
status_t session_balance(money_t *out_balance) {
    if (out_balance == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    Message req;
    Message resp;
    prepare_request(&req, MSG_BALANCE);

    status_t st = send_and_receive(&req, &resp);
    if (st == STATUS_OK) {
        *out_balance = resp.amount;
    }

    return st;
}

/*
 * Submits a cash deposit request. Unless the server was started with --auto-approve,
 * this returns STATUS_TRANSACTION_PENDING along with the generated txn_id so the
 * user can poll later while waiting for operator approval.
 */
status_t session_deposit(money_t amount, money_t *out_balance, char *out_txn_id, size_t txn_len) {
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    Message req;
    Message resp;
    prepare_request(&req, MSG_DEPOSIT);
    req.amount = amount;

    status_t st = send_and_receive(&req, &resp);
    if (out_balance != NULL) {
        *out_balance = resp.amount;
    }
    if (out_txn_id != NULL && txn_len > 0) {
        strncpy(out_txn_id, resp.txn_id, txn_len - 1);
        out_txn_id[txn_len - 1] = '\0';
    }

    return st;
}

/* Submits an immediate withdrawal request and passes back the updated balance */
status_t session_withdraw(money_t amount, money_t *out_balance, char *out_txn_id, size_t txn_len) {
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    Message req;
    Message resp;
    prepare_request(&req, MSG_WITHDRAW);
    req.amount = amount;

    status_t st = send_and_receive(&req, &resp);
    if (out_balance != NULL) {
        *out_balance = resp.amount;
    }
    if (out_txn_id != NULL && txn_len > 0) {
        strncpy(out_txn_id, resp.txn_id, txn_len - 1);
        out_txn_id[txn_len - 1] = '\0';
    }

    return st;
}

/* Submits an atomic transfer from our authenticated account to receiver_id */
status_t session_transfer(const char *receiver_id, money_t amount, money_t *out_balance, char *out_txn_id, size_t txn_len) {
    if (receiver_id == NULL || receiver_id[0] == '\0') {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    Message req;
    Message resp;
    prepare_request(&req, MSG_TRANSFER);
    req.amount = amount;
    strncpy(req.receiver_id, receiver_id, CLIENT_ID_LENGTH);
    req.receiver_id[CLIENT_ID_LENGTH] = '\0';

    status_t st = send_and_receive(&req, &resp);
    if (out_balance != NULL) {
        *out_balance = resp.amount;
    }
    if (out_txn_id != NULL && txn_len > 0) {
        strncpy(out_txn_id, resp.txn_id, txn_len - 1);
        out_txn_id[txn_len - 1] = '\0';
    }

    return st;
}

/*
 * Polls the server for the outcome of an earlier request_id.
 *
 * Why we construct req manually here instead of calling prepare_request():
 *   1. The server looks up the historical transaction by matching req.request_id
 *      against its idempotency cache, so req.request_id must carry the target
 *      request ID we are asking about—not a newly incremented sequence number.
 *   2. Leaving g_session.next_request_id and g_session.last_request_id untouched
 *      ensures polling status never shifts our monotonic counter or overwrites
 *      the record of the last real financial request.
 *
 * Note on return values:
 *   - The function return value (st) reports whether the status query itself
 *     succeeded (e.g., STATUS_OK if the request_id was found in the server cache,
 *     or STATUS_NOT_FOUND if it doesn't exist / was evicted).
 *   - When st == STATUS_OK, resp.status_code holds the historical transaction's
 *     outcome (such as STATUS_OK, STATUS_TRANSACTION_PENDING, or STATUS_INSUFFICIENT_FUNDS),
 *     which we pass back via out_txn_status.
 */
status_t session_status_query(uint32_t request_id, status_t *out_txn_status, money_t *out_balance, char *out_txn_id, size_t txn_len) {
    if (request_id == 0) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    Message req;
    memset(&req, 0, sizeof(req));
    req.mtype      = MSG_REQUEST_TYPE;
    req.request_id = request_id;
    req.sender_pid = (int32_t)g_session.pid;
    req.operation  = (int32_t)MSG_STATUS_QUERY;

    snprintf(req.client_id, sizeof(req.client_id), "%.*s", CLIENT_ID_LEN - 1, g_session.client_id);
    snprintf(req.session_id, sizeof(req.session_id), "%.*s", SESSION_ID_LEN - 1, g_session.session_id);

    Message resp;
    status_t st = send_and_receive(&req, &resp);
    if (st == STATUS_OK) {
        if (out_txn_status != NULL) {
            *out_txn_status = (status_t)resp.status_code;
        }
        if (out_balance != NULL) {
            *out_balance = resp.amount;
        }
        if (out_txn_id != NULL && txn_len > 0) {
            strncpy(out_txn_id, resp.txn_id, txn_len - 1);
            out_txn_id[txn_len - 1] = '\0';
        }
    }

    return st;
}

