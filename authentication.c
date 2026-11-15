#include "server_internal.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server - Authentication
 * Owner: @William (Win/ipc/server)
 *
 * Validates client login requests against accounts present in shared memory
 * (ACC0001..ACC0003) and issues unique session_id_t identifiers.
 * ============================================================================ */

static volatile LONG g_session_seq = 0;

static int account_exists(server_context_t *ctx, const client_id_t *client_id) {
    if (!ctx || !ctx->bank || !client_id) {
        return 0;
    }

    if (ctx->sync_enabled && ctx->sem_handle) {
        DWORD w = WaitForSingleObject(ctx->sem_handle, INFINITE);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
            return 0;
        }
    }

    int found = 0;
    for (uint32_t i = 0; i < ACCOUNT_COUNT; i++) {
        if (strncmp(ctx->bank->accounts[i].client_id.value,
                    client_id->value,
                    CLIENT_ID_LENGTH) == 0) {
            found = 1;
            break;
        }
    }

    if (ctx->sync_enabled && ctx->sem_handle) {
        ReleaseSemaphore(ctx->sem_handle, 1, NULL);
    }

    return found;
}

status_t auth_handle_login(server_context_t *ctx,
                           const login_request_t *req,
                           session_id_t *out_session) {
    if (!ctx || !req || !out_session) {
        return STATUS_INVALID_ARGUMENT;
    }
    if (req->client_id.value[0] == '\0') {
        return STATUS_AUTH_FAILED;
    }
    if (!account_exists(ctx, &req->client_id)) {
        return STATUS_ACCOUNT_NOT_FOUND;
    }

    LONG seq = InterlockedIncrement(&g_session_seq);
    memset(out_session, 0, sizeof(*out_session));
    snprintf(out_session->value, sizeof(out_session->value),
             "SESS-%04lu-%06ld",
             (unsigned long)GetCurrentProcessId(),
             (long)seq);

    return registry_register(out_session, &req->client_id);
}

status_t auth_handle_logout(const session_id_t *session_id) {
    return registry_remove(session_id);
}

status_t auth_require_session(const session_id_t *session_id, client_id_t *out_client_id) {
    return registry_lookup(session_id, out_client_id);
}
