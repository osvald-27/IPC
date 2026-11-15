#include "server.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Authenticates client login requests.
 * Each client session is established upon presenting a valid registered account ID.
 * The server assigns a unique session ID: S<worker>-<pid>-<time>-<counter>
 */
status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, pid_t client_pid) {
    if (ctx == NULL || session == NULL || client_id_str == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    static unsigned long counter = 0;

    client_id_t id;
    memset(&id, 0, sizeof(id));
    strncpy(id.value, client_id_str, CLIENT_ID_LENGTH);
    id.value[CLIENT_ID_LENGTH] = '\0';

    if (registry_find(ctx->bank, &id) == NULL) {
        return STATUS_AUTH_FAILED;
    }

    session->logged_in = 1;
    session->client_id = id;
    session->client_pid = client_pid;
    session->login_time = (uint64_t)time(NULL);

    memset(&session->session_id, 0, sizeof(session->session_id));
    snprintf(session->session_id.value, sizeof(session->session_id.value),
             "S%02d-%ld-%ld-%lu",
             ctx->worker_id, (long)getpid(), (long)time(NULL), ++counter);

    return STATUS_OK;
}

status_t auth_check(const session_t *session, const char *client_id_str, const char *session_id_str) {
    if (session == NULL || !session->logged_in) {
        return STATUS_SESSION_INVALID;
    }

    if (client_id_str != NULL && client_id_str[0] != '\0') {
        if (strncmp(session->client_id.value, client_id_str, CLIENT_ID_LENGTH) != 0) {
            return STATUS_SESSION_INVALID;
        }
    }

    if (session_id_str != NULL && session_id_str[0] != '\0') {
        if (strncmp(session->session_id.value, session_id_str, MAX_SESSION_ID) != 0) {
            return STATUS_SESSION_INVALID;
        }
    }

    return STATUS_OK;
}

void auth_logout(session_t *session) {
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
    }
}
