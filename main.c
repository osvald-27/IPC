#include "server_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * CrossIPC Windows Server Entry Point
 * Owner: @William (Win/ipc/server)
 *
 * Usage:
 *   server.exe [--no-sync]
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Usage: %s [--no-sync] [--help]\n", prog);
    printf("  --no-sync   Disable semaphore locking (demonstrates lost updates)\n");
}

int main(int argc, char *argv[]) {
    int sync_enabled = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-sync") == 0) {
            sync_enabled = 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    server_context_t ctx;
    if (server_init(&ctx, sync_enabled) != STATUS_OK) {
        server_shutdown(&ctx);
        return EXIT_FAILURE;
    }

    server_run(&ctx);
    server_shutdown(&ctx);
    return EXIT_SUCCESS;
}
