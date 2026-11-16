#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../common/types.h"
#include "../common/status.h"
#include "../common/win_protocol.h"
#include "../named_pipe/named_pipe.h"

typedef struct {
    named_pipe_handle_t pipe;
    uint32_t pid;
    int logged_in;
    char client_id[CLIENT_ID_LEN];
    char session_id[SESSION_ID_LEN];
    uint32_t next_request_id;
    uint32_t last_request_id;
    char last_txn_id[TXN_ID_LEN];
} client_session_t;
static client_session_t g_session;

static void prepare_request(Message *req, msg_operation_t op) {
    memset(req, 0, sizeof(*req));
    req->request_id = g_session.next_request_id++;
    req->sender_pid = g_session.pid;
    req->operation = (int32_t)op;
    snprintf(req->client_id, sizeof(req->client_id), "%s", g_session.client_id);
    snprintf(req->session_id, sizeof(req->session_id), "%s", g_session.session_id);
    g_session.last_request_id = req->request_id;
}
static status_t send_and_receive(const Message *req, Message *resp) {
    if (!req || !resp || !g_session.pipe) return STATUS_SERVER_UNAVAILABLE;
    memset(resp, 0, sizeof(*resp));
    if (named_pipe_exchange(g_session.pipe, req, resp) != 0) return STATUS_SERVER_UNAVAILABLE;
    if (resp->txn_id[0]) snprintf(g_session.last_txn_id, sizeof(g_session.last_txn_id), "%s", resp->txn_id);
    return (status_t)resp->status_code;
}
status_t session_connect(const char *unused) {
    (void)unused;
    memset(&g_session, 0, sizeof(g_session));
    g_session.pid = (uint32_t)GetCurrentProcessId();
    g_session.next_request_id = 1;
    if (named_pipe_connect(PIPE_NAME, &g_session.pipe) != 0) return STATUS_SERVER_UNAVAILABLE;
    return STATUS_OK;
}
void session_disconnect(void) {
    named_pipe_close(g_session.pipe);
    memset(&g_session, 0, sizeof(g_session));
}
int session_is_logged_in(void) { return g_session.logged_in; }
const char *session_client_id(void) { return g_session.client_id; }
uint32_t session_last_request_id(void) { return g_session.last_request_id; }
const char *session_last_txn_id(void) { return g_session.last_txn_id; }
status_t session_login(const char *id) {
    if (!id || !*id) return STATUS_INVALID_ARGUMENT;
    memset(g_session.client_id, 0, sizeof(g_session.client_id));
    strncpy(g_session.client_id, id, CLIENT_ID_LENGTH);
    g_session.client_id[CLIENT_ID_LENGTH] = '\0';
    Message req, resp;
    prepare_request(&req, MSG_LOGIN);
    status_t st = send_and_receive(&req, &resp);
    if (st == STATUS_OK) {
        g_session.logged_in = 1;
        snprintf(g_session.session_id, sizeof(g_session.session_id), "%s", resp.session_id);
    } else g_session.client_id[0] = '\0';
    return st;
}
status_t session_logout(void) {
    if (!g_session.logged_in) return STATUS_OK;
    Message req, resp;
    prepare_request(&req, MSG_LOGOUT);
    status_t st = send_and_receive(&req, &resp);
    g_session.logged_in = 0;
    g_session.session_id[0] = '\0';
    return st;
}
status_t session_balance(money_t *out) {
    if (!out) return STATUS_INVALID_ARGUMENT;
    if (!g_session.logged_in) return STATUS_SESSION_INVALID;
    Message req, resp; prepare_request(&req, MSG_BALANCE);
    status_t st = send_and_receive(&req, &resp);
    if (st == STATUS_OK) *out = resp.amount;
    return st;
}
static status_t transaction_request(msg_operation_t op, const char *receiver, money_t amount,
                                    money_t *balance, char *txn, size_t txn_len) {
    if (!g_session.logged_in) return STATUS_SESSION_INVALID;
    if (amount <= 0) return STATUS_INVALID_AMOUNT;
    Message req, resp; prepare_request(&req, op); req.amount = amount;
    if (receiver) snprintf(req.receiver_id, sizeof(req.receiver_id), "%s", receiver);
    status_t st = send_and_receive(&req, &resp);
    if (balance) *balance = resp.amount;
    if (txn && txn_len) snprintf(txn, txn_len, "%s", resp.txn_id);
    return st;
}
status_t session_deposit(money_t a, money_t *b, char *t, size_t n) { return transaction_request(MSG_DEPOSIT, NULL, a, b, t, n); }
status_t session_withdraw(money_t a, money_t *b, char *t, size_t n) { return transaction_request(MSG_WITHDRAW, NULL, a, b, t, n); }
status_t session_transfer(const char *r, money_t a, money_t *b, char *t, size_t n) {
    if (!r || r[0] == '\0') return STATUS_INVALID_ARGUMENT;
    return transaction_request(MSG_TRANSFER, r, a, b, t, n);
}
status_t session_status_query(uint32_t id, status_t *out_status, money_t *out_balance, char *out_txn, size_t n) {
    if (!id) return STATUS_INVALID_ARGUMENT;
    if (!g_session.logged_in) return STATUS_SESSION_INVALID;
    Message req, resp; memset(&req, 0, sizeof(req));
    req.request_id = id; req.sender_pid = g_session.pid; req.operation = MSG_STATUS_QUERY;
    snprintf(req.client_id, sizeof(req.client_id), "%s", g_session.client_id);
    snprintf(req.session_id, sizeof(req.session_id), "%s", g_session.session_id);
    status_t st = send_and_receive(&req, &resp);
    if (st == STATUS_OK) {
        if (out_status) *out_status = (status_t)resp.status_code;
        if (out_balance) *out_balance = resp.amount;
        if (out_txn && n) snprintf(out_txn, n, "%s", resp.txn_id);
    }
    return st;
}
