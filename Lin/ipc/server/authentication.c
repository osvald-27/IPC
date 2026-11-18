#include "server.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, pid_t client_pid) {
    if (ctx == NULL || ctx->bank == NULL || session == NULL || client_id_str == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    static unsigned long counter = 0;

    client_id_t id;
    memset(&id, 0, sizeof(id));
    snprintf(id.value, sizeof(id.value), "%.*s", (int)CLIENT_ID_LENGTH, client_id_str);

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    if (registry_find(ctx->bank, &id) == NULL) {
        if (ctx->use_lock) semaphore_unlock(ctx->lock);
        return STATUS_AUTH_FAILED;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!ctx->bank->sessions[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        slot = (int)(counter % MAX_SESSIONS);
    }

    session->logged_in = 1;
    session->client_id = id;
    session->client_pid = client_pid;
    session->login_time = (uint64_t)time(NULL);

    memset(&session->session_id, 0, sizeof(session->session_id));
    snprintf(session->session_id.value, sizeof(session->session_id.value),
             "S%02d-%ld-%ld-%lu",
             ctx->worker_id, (long)getpid(), (long)time(NULL), ++counter);

    ctx->bank->sessions[slot].active     = 1;
    ctx->bank->sessions[slot].client_id  = id;
    ctx->bank->sessions[slot].session_id = session->session_id;
    ctx->bank->sessions[slot].client_pid = (int32_t)client_pid;
    ctx->bank->sessions[slot].login_time = session->login_time;

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    return STATUS_OK;
}


status_t auth_check(server_ctx_t *ctx, const char *client_id_str, const char *session_id_str)
{
    if (ctx == NULL || ctx->bank == NULL || client_id_str == NULL || session_id_str == NULL) {
        return STATUS_SESSION_INVALID;
    }

    if (client_id_str[0] == '\0' || session_id_str[0] == '\0') {
        return STATUS_SESSION_INVALID;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return STATUS_SYNCHRONIZATION_ERROR;
    }

    status_t result = STATUS_SESSION_INVALID;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (ctx->bank->sessions[i].active &&
            strncmp(ctx->bank->sessions[i].client_id.value, client_id_str, CLIENT_ID_LENGTH) == 0 &&
            strncmp(ctx->bank->sessions[i].session_id.value, session_id_str, MAX_SESSION_ID) == 0) {
            result = STATUS_OK;
            break;
        }
    }

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }

    return result;
}

void auth_logout(server_ctx_t *ctx, const char *session_id_str)
{
    if (ctx == NULL || ctx->bank == NULL || session_id_str == NULL || session_id_str[0] == '\0') {
        return;
    }

    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) {
        return;
    }

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (ctx->bank->sessions[i].active &&
            strncmp(ctx->bank->sessions[i].session_id.value, session_id_str, MAX_SESSION_ID) == 0) {
            memset(&ctx->bank->sessions[i], 0, sizeof(ctx->bank->sessions[i]));
            break;
        }
    }

    if (ctx->use_lock) {
        semaphore_unlock(ctx->lock);
    }
}

