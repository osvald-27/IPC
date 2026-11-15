
#include <stdio.h>
#include <stddef.h>
#include <conio.h>

/* Read a password without displaying the characters. */
int session_read_password(char *password, size_t capacity)
{
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
