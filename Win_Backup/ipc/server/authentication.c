#include "server.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, uint32_t client_pid) {
    if (!ctx || !ctx->bank || !session || !client_id_str) return STATUS_INVALID_ARGUMENT;
    static LONG counter = 0;
    client_id_t id; memset(&id, 0, sizeof(id));
    snprintf(id.value, sizeof(id.value), "%.*s", (int)CLIENT_ID_LENGTH, client_id_str);
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) return STATUS_SYNCHRONIZATION_ERROR;
    if (!registry_find(ctx->bank, &id)) {
        if (ctx->use_lock) semaphore_unlock(ctx->lock);
        return STATUS_AUTH_FAILED;
    }
    int slot = -1;
    for (int i=0; i<MAX_SESSIONS; ++i) if (!ctx->bank->sessions[i].active) { slot=i; break; }
    LONG seq = InterlockedIncrement(&counter);
    /* Match Linux's bounded session table: recycle the next slot if all are occupied. */
    if (slot < 0) slot = (int)((seq - 1) % MAX_SESSIONS);
    memset(session, 0, sizeof(*session));
    session->logged_in = 1; session->client_id = id; session->client_pid = client_pid;
    session->login_time = (uint64_t)time(NULL);
    /* Fixed-width hexadecimal fields keep the token below the 36-byte pipe field. */
    snprintf(session->session_id.value, sizeof(session->session_id.value), "S%02d-%08lX-%08lX-%08lX",
             ctx->worker_id, (unsigned long)GetCurrentProcessId(),
             (unsigned long)(uint32_t)session->login_time, (unsigned long)(uint32_t)seq);
    session_entry_t *entry = &ctx->bank->sessions[slot];
    entry->active=1; entry->client_id=id; entry->session_id=session->session_id;
    entry->client_pid=(int32_t)client_pid; entry->login_time=session->login_time;
    if (ctx->use_lock) semaphore_unlock(ctx->lock);
    return STATUS_OK;
}

status_t auth_check(server_ctx_t *ctx, const char *client_id, const char *session_id) {
    if (!ctx || !ctx->bank || !client_id || !session_id || !*client_id || !*session_id) return STATUS_SESSION_INVALID;
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) return STATUS_SYNCHRONIZATION_ERROR;
    status_t result = STATUS_SESSION_INVALID;
    for (int i=0; i<MAX_SESSIONS; ++i) {
        session_entry_t *e=&ctx->bank->sessions[i];
        if (e->active && strncmp(e->client_id.value,client_id,CLIENT_ID_LENGTH)==0 &&
            strncmp(e->session_id.value,session_id,MAX_SESSION_ID)==0) { result=STATUS_OK; break; }
    }
    if (ctx->use_lock) semaphore_unlock(ctx->lock);
    return result;
}

void auth_logout(server_ctx_t *ctx, const char *session_id) {
    if (!ctx || !ctx->bank || !session_id || !*session_id) return;
    if (ctx->use_lock && semaphore_lock(ctx->lock) != STATUS_OK) return;
    for (int i=0; i<MAX_SESSIONS; ++i) {
        session_entry_t *e=&ctx->bank->sessions[i];
        if (e->active && strncmp(e->session_id.value,session_id,MAX_SESSION_ID)==0) { memset(e,0,sizeof(*e)); break; }
    }
    if (ctx->use_lock) semaphore_unlock(ctx->lock);
}
