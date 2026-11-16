/*
 * session.c (Windows Client Session & Named Pipe Adapter)
 * Same public session_*() functions as the Linux version, but the
 * transport is a Windows named pipe instead of a System V queue.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../common/types.h"
#include "../common/status.h"
#include "../common/protocol.h"
#include "../named_pipe/named_pipe.h"

typedef struct {
    named_pipe_t *pipe;
    int           logged_in;
    client_id_t   client_id;
    session_id_t  session_id;
    uint32_t      next_request_id;
    uint32_t      last_request_id;
    char          last_txn_id[MAX_TRANSACTION_ID + 1U];
} client_session_t;

static client_session_t g_session;

/* Unique per process: PREFIX-<process id>-<sequence>. */
static void make_request_id(request_id_t *out, const char *prefix,
                            uint32_t seq)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->value, sizeof(out->value), "%s-%08lX-%08lu",
             prefix, (unsigned long)GetCurrentProcessId(),
             (unsigned long)seq);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    snprintf(dst, dst_size, "%s", src);
}

/*
 * Send one request and read the matching reply.
 * Reply layout: response_t followed by response_t.payload_size bytes.
 */
static status_t exchange(uint32_t message_type,
                         const request_id_t *rid,
                         const void *payload, uint32_t payload_size,
                         response_t *resp,
                         void *data, uint32_t data_capacity)
{
    message_header_t req_h;
    message_header_t resp_h;
    uint8_t received[MAX_PAYLOAD_SIZE];
    status_t st;

    if (g_session.pipe == NULL || rid == NULL || resp == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(&req_h, 0, sizeof(req_h));
    memset(&resp_h, 0, sizeof(resp_h));
    memset(resp, 0, sizeof(*resp));

    req_h.magic        = PROTOCOL_NUMBER;
    req_h.version      = PROTOCOL_VERSION;
    req_h.header_size  = (uint16_t)sizeof(req_h);
    req_h.message_type = message_type;
    req_h.payload_size = payload_size;
    req_h.request_id   = *rid;
    req_h.session_id   = g_session.session_id;

    st = pipe_send_message(g_session.pipe, &req_h, payload);
    if (st != STATUS_OK) {
        return st;
    }

    st = pipe_recv_message(g_session.pipe, &resp_h, received,
                           MAX_PAYLOAD_SIZE);
    if (st != STATUS_OK) {
        return st;
    }

    if (resp_h.magic != PROTOCOL_NUMBER ||
        resp_h.message_type != RESPONSE_MESSAGE ||
        strcmp(resp_h.request_id.value, rid->value) != 0 ||
        resp_h.payload_size < sizeof(response_t) ||
        resp_h.payload_size > MAX_PAYLOAD_SIZE) {
        return STATUS_ERROR;
    }

    memcpy(resp, received, sizeof(response_t));

    if (resp->payload_size > resp_h.payload_size - sizeof(response_t) ||
        resp->payload_size > data_capacity) {
        return STATUS_ERROR;
    }
    if (resp->payload_size > 0U) {
        if (data == NULL) {
            return STATUS_INVALID_ARGUMENT;
        }
        memcpy(data, received + sizeof(response_t), resp->payload_size);
    }
    return STATUS_OK;   /* transport worked; resp->status is the server's answer */
}

/* Build the next request ID and remember it for 'status'. */
static void next_request(request_id_t *rid)
{
    uint32_t seq = g_session.next_request_id++;
    make_request_id(rid, "REQ", seq);
    g_session.last_request_id = seq;
}

status_t session_connect(const char *key_path)
{
    status_t st;
    (void)key_path;   /* Linux uses a key file; Windows uses PIPE_NAME */

    memset(&g_session, 0, sizeof(g_session));
    g_session.next_request_id = 1;   /* 0 means "no request yet" */

    st = pipe_client_open(PIPE_NAME, 5000U, &g_session.pipe);
    if (st == STATUS_NOT_FOUND || st == STATUS_TIMEOUT) {
        g_session.pipe = NULL;
        return STATUS_SERVER_UNAVAILABLE;
    }
    if (st != STATUS_OK) {
        g_session.pipe = NULL;
    }
    return st;
}

void session_disconnect(void)
{
    if (g_session.pipe != NULL) {
        pipe_close(g_session.pipe);
    }
    memset(&g_session, 0, sizeof(g_session));
}

int session_is_logged_in(void)
{
    return g_session.logged_in;
}

const char *session_client_id(void)
{
    return g_session.client_id.value;
}

uint32_t session_last_request_id(void)
{
    return g_session.last_request_id;
}

const char *session_last_txn_id(void)
{
    return g_session.last_txn_id;
}

status_t session_login(const char *client_id_str)
{
    login_request_t body;
    request_id_t rid;
    response_t resp;
    session_id_t issued;
    status_t st;

    if (client_id_str == NULL || client_id_str[0] == '\0' ||
        strlen(client_id_str) > CLIENT_ID_LENGTH) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(&g_session.client_id, 0, sizeof(g_session.client_id));
    copy_text(g_session.client_id.value, sizeof(g_session.client_id.value),
              client_id_str);

    memset(&body, 0, sizeof(body));
    memset(&issued, 0, sizeof(issued));
    body.client_id = g_session.client_id;
    next_request(&rid);

    st = exchange(REQUEST_LOGIN, &rid, &body, sizeof(body),
                  &resp, &issued, sizeof(issued));
    if (st == STATUS_OK) {
        st = resp.status;
    }
    if (st == STATUS_OK && resp.payload_size == sizeof(session_id_t) &&
        issued.value[0] != '\0') {
        g_session.session_id = issued;
        g_session.logged_in = 1;
        return STATUS_OK;
    }
    if (st == STATUS_OK) {
        st = STATUS_ERROR;   /* server said OK but sent no session */
    }
    memset(&g_session.client_id, 0, sizeof(g_session.client_id));
    g_session.logged_in = 0;
    return st;
}

status_t session_logout(void)
{
    request_id_t rid;
    response_t resp;
    status_t st;

    if (!g_session.logged_in) {
        return STATUS_OK;
    }
    next_request(&rid);
    st = exchange(REQUEST_LOGOUT, &rid, NULL, 0U, &resp, NULL, 0U);
    if (st == STATUS_OK) {
        st = resp.status;
    }
    g_session.logged_in = 0;
    memset(&g_session.session_id, 0, sizeof(g_session.session_id));
    return st;
}

status_t session_balance(money_t *out_balance)
{
    balance_request_t body;
    balance_response_t reply;
    request_id_t rid;
    response_t resp;
    status_t st;

    if (out_balance == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    memset(&body, 0, sizeof(body));
    memset(&reply, 0, sizeof(reply));
    body.client_id = g_session.client_id;
    next_request(&rid);

    st = exchange(REQUEST_GET_BALANCE, &rid, &body, sizeof(body),
                  &resp, &reply, sizeof(reply));
    if (st != STATUS_OK) {
        return st;
    }
    if (resp.status != STATUS_OK) {
        return resp.status;
    }
    if (resp.payload_size != sizeof(balance_response_t)) {
        return STATUS_ERROR;
    }
    *out_balance = reply.balance;
    return STATUS_OK;
}

/* Shared by deposit, withdraw and transfer. */
static status_t run_transaction(uint32_t message_type,
                                transaction_type_t type,
                                const char *receiver_id,
                                money_t amount,
                                money_t *out_balance,
                                char *out_txn_id, size_t txn_len)
{
    transaction_request_t body;
    transaction_response_t reply;
    request_id_t rid;
    response_t resp;
    status_t st;

    memset(&body, 0, sizeof(body));
    memset(&reply, 0, sizeof(reply));

    body.type      = type;
    body.sender_id = g_session.client_id;
    body.amount    = amount;
    if (receiver_id != NULL) {
        copy_text(body.receiver_id.value, sizeof(body.receiver_id.value),
                  receiver_id);
    }
    next_request(&rid);

    st = exchange(message_type, &rid, &body, sizeof(body),
                  &resp, &reply, sizeof(reply));
    if (st != STATUS_OK) {
        return st;
    }

    if (resp.payload_size == sizeof(transaction_response_t)) {
        if (out_balance != NULL) {
            *out_balance = reply.balance;
        }
        if (out_txn_id != NULL && txn_len > 0) {
            copy_text(out_txn_id, txn_len, reply.transaction_id.value);
        }
        if (reply.transaction_id.value[0] != '\0') {
            copy_text(g_session.last_txn_id, sizeof(g_session.last_txn_id),
                      reply.transaction_id.value);
        }
    }
    return resp.status;   /* OK, PENDING, INSUFFICIENT_FUNDS, ... */
}

status_t session_deposit(money_t amount, money_t *out_balance,
                         char *out_txn_id, size_t txn_len)
{
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }
    return run_transaction(REQUEST_DEPOSIT, TRANSACTION_DEPOSIT, NULL,
                           amount, out_balance, out_txn_id, txn_len);
}

status_t session_withdraw(money_t amount, money_t *out_balance,
                          char *out_txn_id, size_t txn_len)
{
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }
    return run_transaction(REQUEST_WITHDRAWAL, TRANSACTION_WITHDRAWAL, NULL,
                           amount, out_balance, out_txn_id, txn_len);
}

status_t session_transfer(const char *receiver_id, money_t amount,
                          money_t *out_balance,
                          char *out_txn_id, size_t txn_len)
{
    if (receiver_id == NULL || receiver_id[0] == '\0' ||
        strlen(receiver_id) > CLIENT_ID_LENGTH) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }
    return run_transaction(REQUEST_TRANSFER, TRANSACTION_TRANSFER,
                           receiver_id, amount, out_balance,
                           out_txn_id, txn_len);
}

/*
 * Ask the server about an earlier request. The payload is the ID of that
 * earlier request; the header ID is a separate "QRY" ID, so the counters
 * and last_request_id are not touched.
 * Return value = did the lookup work; *out_txn_status = how that request ended.
 */
status_t session_status_query(uint32_t request_id,
                              status_t *out_txn_status,
                              money_t *out_balance,
                              char *out_txn_id, size_t txn_len)
{
    request_id_t target;
    request_id_t rid;
    transaction_response_t reply;
    response_t resp;
    status_t st;

    if (request_id == 0) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    make_request_id(&target, "REQ", request_id);
    make_request_id(&rid, "QRY", request_id);
    memset(&reply, 0, sizeof(reply));

    st = exchange(REQUEST_TRANSACTION_STATUS, &rid, &target, sizeof(target),
                  &resp, &reply, sizeof(reply));
    if (st != STATUS_OK) {
        return st;
    }
    if (resp.status != STATUS_OK) {
        return resp.status;   /* e.g. STATUS_NOT_FOUND */
    }
    if (resp.payload_size != sizeof(transaction_response_t)) {
        return STATUS_ERROR;
    }

    if (out_txn_status != NULL) {
        *out_txn_status = reply.status;
    }
    if (out_balance != NULL) {
        *out_balance = reply.balance;
    }
    if (out_txn_id != NULL && txn_len > 0) {
        copy_text(out_txn_id, txn_len, reply.transaction_id.value);
    }
    return STATUS_OK;
}
