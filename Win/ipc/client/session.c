#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../server/server.h"
#include "../named_pipe/named_pipe.h"

typedef struct {
    HANDLE pipe;
    uint32_t next_request_id;
    uint32_t last_request_id;
    int logged_in;
    char client_id[CLIENT_ID_LEN];
    char session_id[SESSION_ID_LEN];
    char last_txn_id[TXN_ID_LEN];
} client_session_t;

static client_session_t g_session;

static void session_copy(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U) {
        return;
    }

    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    strncpy(destination, source, capacity - 1U);
    destination[capacity - 1U] = '\0';
}

static void session_prepare(Message *request, int operation)
{
    memset(request, 0, sizeof(*request));

    request->request_id = g_session.next_request_id++;
    request->sender_pid = GetCurrentProcessId();
    request->operation = operation;

    session_copy(
        request->client_id,
        sizeof(request->client_id),
        g_session.client_id
    );

    session_copy(
        request->session_id,
        sizeof(request->session_id),
        g_session.session_id
    );

    g_session.last_request_id = request->request_id;
}

static status_t session_exchange(const Message *request, Message *response)
{
    status_t status;

    if (request == NULL || response == NULL ||
        g_session.pipe == INVALID_HANDLE_VALUE ||
        g_session.pipe == NULL) {
        return STATUS_SERVER_UNAVAILABLE;
    }

    status = named_pipe_write_exact(
        g_session.pipe,
        request,
        sizeof(*request)
    );

    if (status != STATUS_OK) {
        return status;
    }

    memset(response, 0, sizeof(*response));

    status = named_pipe_read_exact(
        g_session.pipe,
        response,
        sizeof(*response)
    );

    if (status != STATUS_OK) {
        return status;
    }

    if (response->request_id != request->request_id) {
        return STATUS_QUEUE_ERROR;
    }

    if (response->txn_id[0] != '\0') {
        session_copy(
            g_session.last_txn_id,
            sizeof(g_session.last_txn_id),
            response->txn_id
        );
    }

    return (status_t)response->status_code;
}

status_t session_connect(void)
{
    memset(&g_session, 0, sizeof(g_session));
    g_session.pipe = INVALID_HANDLE_VALUE;
    g_session.next_request_id = 1U;

    return named_pipe_client_connect(&g_session.pipe);
}

void session_disconnect(void)
{
    if (g_session.pipe != INVALID_HANDLE_VALUE && g_session.pipe != NULL) {
        CloseHandle(g_session.pipe);
    }

    SecureZeroMemory(&g_session, sizeof(g_session));
    g_session.pipe = INVALID_HANDLE_VALUE;
}

int session_is_logged_in(void)
{
    return g_session.logged_in;
}

const char *session_client_id(void)
{
    return g_session.client_id;
}

uint32_t session_last_request_id(void)
{
    return g_session.last_request_id;
}

const char *session_last_txn_id(void)
{
    return g_session.last_txn_id;
}

status_t session_login(const char *client_id)
{
    Message request;
    Message response;
    status_t status;

    if (client_id == NULL || strlen(client_id) != CLIENT_ID_LENGTH) {
        return STATUS_INVALID_ARGUMENT;
    }

    session_copy(
        g_session.client_id,
        sizeof(g_session.client_id),
        client_id
    );

    session_prepare(&request, MSG_LOGIN);
    status = session_exchange(&request, &response);

    if (status == STATUS_OK) {
        session_copy(
            g_session.session_id,
            sizeof(g_session.session_id),
            response.session_id
        );
        g_session.logged_in = 1;
    } else {
        SecureZeroMemory(g_session.client_id, sizeof(g_session.client_id));
        SecureZeroMemory(g_session.session_id, sizeof(g_session.session_id));
        g_session.logged_in = 0;
    }

    return status;
}

status_t session_logout(void)
{
    Message request;
    Message response;
    status_t status;

    if (!g_session.logged_in) {
        return STATUS_OK;
    }

    session_prepare(&request, MSG_LOGOUT);
    status = session_exchange(&request, &response);

    g_session.logged_in = 0;
    SecureZeroMemory(g_session.session_id, sizeof(g_session.session_id));

    return status;
}

status_t session_balance(money_t *balance)
{
    Message request;
    Message response;
    status_t status;

    if (balance == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    session_prepare(&request, MSG_BALANCE);
    status = session_exchange(&request, &response);

    if (status == STATUS_OK) {
        *balance = response.amount;
    }

    return status;
}

status_t session_deposit(
    money_t amount,
    money_t *balance,
    char *transaction_id,
    size_t transaction_id_capacity
)
{
    Message request;
    Message response;
    status_t status;

    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    session_prepare(&request, MSG_DEPOSIT);
    request.amount = amount;

    status = session_exchange(&request, &response);

    if (balance != NULL) {
        *balance = response.amount;
    }

    if (transaction_id != NULL && transaction_id_capacity > 0U) {
        session_copy(transaction_id, transaction_id_capacity, response.txn_id);
    }

    return status;
}

status_t session_withdraw(
    money_t amount,
    money_t *balance,
    char *transaction_id,
    size_t transaction_id_capacity
)
{
    Message request;
    Message response;
    status_t status;

    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    session_prepare(&request, MSG_WITHDRAW);
    request.amount = amount;

    status = session_exchange(&request, &response);

    if (balance != NULL) {
        *balance = response.amount;
    }

    if (transaction_id != NULL && transaction_id_capacity > 0U) {
        session_copy(transaction_id, transaction_id_capacity, response.txn_id);
    }

    return status;
}

status_t session_transfer(
    const char *receiver_id,
    money_t amount,
    money_t *balance,
    char *transaction_id,
    size_t transaction_id_capacity
)
{
    Message request;
    Message response;
    status_t status;

    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    if (receiver_id == NULL || strlen(receiver_id) != CLIENT_ID_LENGTH) {
        return STATUS_INVALID_ARGUMENT;
    }

    if (amount <= 0) {
        return STATUS_INVALID_AMOUNT;
    }

    session_prepare(&request, MSG_TRANSFER);
    request.amount = amount;

    session_copy(
        request.receiver_id,
        sizeof(request.receiver_id),
        receiver_id
    );

    status = session_exchange(&request, &response);

    if (balance != NULL) {
        *balance = response.amount;
    }

    if (transaction_id != NULL && transaction_id_capacity > 0U) {
        session_copy(transaction_id, transaction_id_capacity, response.txn_id);
    }

    return status;
}

status_t session_status_query(
    uint32_t request_id,
    status_t *transaction_status,
    money_t *balance,
    char *transaction_id,
    size_t transaction_id_capacity
)
{
    Message request;
    Message response;
    status_t status;

    if (!g_session.logged_in) {
        return STATUS_SESSION_INVALID;
    }

    if (request_id == 0U) {
        return STATUS_INVALID_ARGUMENT;
    }

    memset(&request, 0, sizeof(request));

    request.request_id = request_id;
    request.sender_pid = GetCurrentProcessId();
    request.operation = MSG_STATUS_QUERY;

    session_copy(
        request.client_id,
        sizeof(request.client_id),
        g_session.client_id
    );

    session_copy(
        request.session_id,
        sizeof(request.session_id),
        g_session.session_id
    );

    status = session_exchange(&request, &response);

    if (status == STATUS_OK) {
        if (transaction_status != NULL) {
            *transaction_status = (status_t)response.status_code;
        }

        if (balance != NULL) {
            *balance = response.amount;
        }

        if (transaction_id != NULL && transaction_id_capacity > 0U) {
            session_copy(
                transaction_id,
                transaction_id_capacity,
                response.txn_id
            );
        }
    }

    return status;
}
