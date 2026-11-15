#include "server.h"

#include <stdio.h>
#include <string.h>

/*
 * A client logs in with its account id and gets a session id back.
 * Every later request must carry that session id in its header.
 *
 * One worker serves one client connection from login to logout, so
 * the session lives in the worker's own memory and needs no lock.
 *
 * Not done here: the password and encryption part of the design
 * document (Argon2id, AES-256-GCM). Login is by account id only.
 */

status_t auth_login(server_ctx_t *ctx, session_t *session, const login_request_t *request) {
    static unsigned long counter = 0;

    client_id_t id = request->client_id;
    id.value[CLIENT_ID_LENGTH] = '\0';      /* never trust the client to terminate it */

    if (registry_find(ctx->bank, &id) == NULL) {
        return STATUS_AUTH_FAILED;
    }

    session->logged_in = 1;
    session->client_id = id;
    memset(&session->session_id, 0, sizeof session->session_id);

    /* worker, process id, tick count and a counter: unique for this run */
    snprintf(session->session_id.value, sizeof session->session_id.value, "S%02d-%lu-%lu-%lu",
             ctx->worker_id, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount(), ++counter);
    return STATUS_OK;
}

status_t auth_check(const session_t *session, const message_header_t *header) {
    if (!session->logged_in) {
        return STATUS_SESSION_INVALID;
    }
    if (strncmp(session->session_id.value, header->session_id.value, MAX_SESSION_ID) != 0) {
        return STATUS_SESSION_INVALID;
    }
    return STATUS_OK;
}

void auth_logout(session_t *session) {
    memset(session, 0, sizeof *session);
}
