#include "server_internal.h"

#include <stdio.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server - Client Registry
 * Owner: @William (Win/ipc/server)
 *
 * Thread-safe table mapping active session_id_t tokens to authenticated
 * client_id_t accounts (up to MAX_CLIENTS).
 * ============================================================================ */

typedef struct {
    int          in_use;
    session_id_t session_id;
    client_id_t  client_id;
} registry_entry_t;

static registry_entry_t g_entries[MAX_CLIENTS];
static CRITICAL_SECTION g_registry_cs;
static int              g_registry_ready = 0;

void registry_init(void) {
    if (!g_registry_ready) {
        InitializeCriticalSection(&g_registry_cs);
        g_registry_ready = 1;
    }
    EnterCriticalSection(&g_registry_cs);
    memset(g_entries, 0, sizeof(g_entries));
    LeaveCriticalSection(&g_registry_cs);
}

void registry_destroy(void) {
    if (g_registry_ready) {
        DeleteCriticalSection(&g_registry_cs);
        g_registry_ready = 0;
    }
}

status_t registry_register(const session_id_t *session_id, const client_id_t *client_id) {
    if (!session_id || !client_id || session_id->value[0] == '\0') {
        return STATUS_INVALID_ARGUMENT;
    }

    EnterCriticalSection(&g_registry_cs);
    int free_idx = -1;
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (!g_entries[i].in_use) {
            if (free_idx == -1) {
                free_idx = (int)i;
            }
        } else if (strncmp(g_entries[i].session_id.value,
                           session_id->value,
                           MAX_SESSION_ID) == 0) {
            LeaveCriticalSection(&g_registry_cs);
            return STATUS_ALREADY_EXISTS;
        }
    }

    if (free_idx == -1) {
        LeaveCriticalSection(&g_registry_cs);
        return STATUS_OUT_OF_MEMORY;
    }

    g_entries[free_idx].in_use = 1;
    g_entries[free_idx].session_id = *session_id;
    g_entries[free_idx].client_id = *client_id;
    LeaveCriticalSection(&g_registry_cs);

    return STATUS_OK;
}

status_t registry_lookup(const session_id_t *session_id, client_id_t *out_client_id) {
    if (!session_id || session_id->value[0] == '\0') {
        return STATUS_SESSION_INVALID;
    }

    EnterCriticalSection(&g_registry_cs);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_entries[i].in_use &&
            strncmp(g_entries[i].session_id.value,
                    session_id->value,
                    MAX_SESSION_ID) == 0) {
            if (out_client_id) {
                *out_client_id = g_entries[i].client_id;
            }
            LeaveCriticalSection(&g_registry_cs);
            return STATUS_OK;
        }
    }
    LeaveCriticalSection(&g_registry_cs);
    return STATUS_SESSION_INVALID;
}

status_t registry_remove(const session_id_t *session_id) {
    if (!session_id || session_id->value[0] == '\0') {
        return STATUS_SESSION_INVALID;
    }

    EnterCriticalSection(&g_registry_cs);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_entries[i].in_use &&
            strncmp(g_entries[i].session_id.value,
                    session_id->value,
                    MAX_SESSION_ID) == 0) {
            memset(&g_entries[i], 0, sizeof(registry_entry_t));
            LeaveCriticalSection(&g_registry_cs);
            return STATUS_OK;
        }
    }
    LeaveCriticalSection(&g_registry_cs);
    return STATUS_NOT_FOUND;
}

uint32_t registry_active_count(void) {
    uint32_t count = 0;
    EnterCriticalSection(&g_registry_cs);
    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_entries[i].in_use) {
            count++;
        }
    }
    LeaveCriticalSection(&g_registry_cs);
    return count;
}
