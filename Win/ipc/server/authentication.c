/*
 * authentication.c
 *
 * Manages per-connection client authentication and session lifecycle.
 *
 * Design notes:
 * - Connection-scoped state: Each named-pipe connection owns its own private
 *   session_t struct on the worker thread handling that pipe. Because sessions
 *   aren't shared across threads, auth_check() and auth_logout() don't need
 *   to acquire the global bank lock.
 * - Collision-free session tokens: Multiple worker threads can accept logins
 *   at the exact same second. Combining the worker ID, server PID, timestamp,
 *   and an atomic counter guarantees every session ID is unique without needing
 *   a mutex or cryptographic RNG.
 * - Identity-based login: In this protocol, presenting a valid registered
 *   account ID is sufficient to open a session (no password/PIN step), which
 *   keeps automated load-testing and multi-client benchmarks lightweight.
 */

#include "server.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Validates a client's account ID against the bank registry and binds an
 * active session to the current pipe connection.
 *
 * Why InterlockedIncrement is used here:
 * 'counter' is shared across all worker threads in the server process. Using
 * Windows atomic InterlockedIncrement() avoids a data race when two workers
 * process logins simultaneously, ensuring no two sessions ever share the
 * same sequence number within the same second.
 */
status_t auth_login(server_ctx_t *ctx, session_t *session, const char *client_id_str, uint32_t client_pid) {
    if (ctx == NULL || session == NULL || client_id_str == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    static LONG counter = 0;
    LONG seq = InterlockedIncrement(&counter);

    /*
     * Normalize the untrusted wire string into a fixed-size, null-terminated
     * client_id_t before searching the registry. This prevents buffer over-reads
     * if a client sends an ID without a trailing null byte.
     */
    client_id_t id;
    memset(&id, 0, sizeof(id));
    strncpy(id.value, client_id_str, CLIENT_ID_LENGTH);
    id.value[CLIENT_ID_LENGTH] = '\0';

    /* Reject login immediately if the account doesn't exist in the registry */
    if (registry_find(ctx->bank, &id) == NULL) {
        return STATUS_AUTH_FAILED;
    }

    /* Record session metadata for auditing and subsequent request validation */
    session->logged_in  = 1;
    session->client_id  = id;
    session->client_pid = client_pid;
    session->login_time = (uint64_t)time(NULL);

    /*
     * Build a human-readable, globally unique session token:
     *   S<worker_id>-<server_pid>-<unix_timestamp>-<atomic_seq>
     * Zeroing first ensures unused buffer bytes are clean before sending back
     * over the pipe.
     */
    memset(&session->session_id, 0, sizeof(session->session_id));
    snprintf(session->session_id.value, sizeof(session->session_id.value),
             "S%02d-%lu-%lld-%ld",
             ctx->worker_id,
             (unsigned long)GetCurrentProcessId(),
             (long long)time(NULL),
             (long)seq);

    return STATUS_OK;
}

/*
 * Verifies that an incoming request belongs to an active session and matches
 * the account and token established at login.
 *
 * Why client_id_str and session_id_str are optional (checked only when non-empty):
 * Some lightweight clients or internal callers rely on the persistent named-pipe
 * connection itself as proof of identity, leaving one or both header fields
 * blank after login. If the client *does* supply them, they must match the
 * bound session—preventing one logged-in client from spoofing transactions
 * against another account's ID.
 */
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

/*
 * Tears down an active session on explicit logout or pipe disconnect.
 *
 * Wiping the entire struct with memset() guarantees logged_in becomes 0 and
 * clears stale client/session IDs so a reused connection struct can never
 * accidentally inherit privileges from a previous client.
 */
void auth_logout(session_t *session) {
    if (session != NULL) {
        memset(session, 0, sizeof(*session));
    }
}
