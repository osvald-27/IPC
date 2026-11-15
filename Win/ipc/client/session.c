

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <conio.h>

#include "../common/types.h"

/* Stores the current client's session information. */
typedef struct session {
    client_id_t client_id;
    session_id_t session_id;
    int authenticated;
} session_t;

/* Read a password without displaying the characters. */
int session_read_password(char *password, size_t capacity) {
    size_t length = 0;
    int ch;

    if (password == NULL || capacity < 2)
        return 0;

    printf("Enter password: ");
    fflush(stdout);

    while (1) {
        ch = _getch();

        if (ch == '\r')
            break;

        if (ch == '\b') {
            if (length > 0)
                length--;
            continue;
        }

        if (ch < 32 || ch > 126)
            continue;

        if (length >= capacity - 1)
            continue;

        password[length++] = (char)ch;
    }

    password[length] = '\0';
    printf("\n");

    return length > 0;
}

/* Initialize an empty session. */
void session_init(session_t *session) {
    if (session == NULL)
        return;

    memset(session, 0, sizeof(session_t));
}

/* Store the client's identity after successful authentication. */
int session_start(session_t *session,
                  const client_id_t *client_id,
                  const session_id_t *session_id) {

    if (session == NULL || client_id == NULL || session_id == NULL)
        return 0;

    if (client_id->value[0] == '\0' ||
        session_id->value[0] == '\0')
        return 0;

    session->client_id = *client_id;
    session->session_id = *session_id;
    session->authenticated = 1;

    return 1;
}

/* Clear the session when the client logs out. */
void session_end(session_t *session) {
    if (session == NULL)
        return;

    memset(session, 0, sizeof(session_t));
}

/* Check whether the client is authenticated. */
int session_is_authenticated(const session_t *session) {
    return session != NULL && session->authenticated;
}
