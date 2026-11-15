#include "server.h"

#include <stdio.h>
#include <string.h>

/*
 * The bank is saved to DATA_FILE when the server shuts down and read
 * back at the next start, so balances survive a restart.
 * File layout: magic, account count, then the bank_t bytes.
 */

#define DATA_FILE_TMP DATA_FILE ".tmp"

typedef struct {
    uint32_t magic;
    uint32_t account_count;
} file_header_t;

status_t persistence_load(bank_t *bank) {
    FILE *f = fopen(DATA_FILE, "rb");
    if (f == NULL) {
        return STATUS_NOT_FOUND;        /* first run, nothing saved yet */
    }

    file_header_t header;
    bank_t loaded;
    int ok = fread(&header, sizeof header, 1, f) == 1 &&
             header.magic == PROTOCOL_NUMBER &&
             header.account_count == ACCOUNT_COUNT &&
             fread(&loaded, sizeof loaded, 1, f) == 1;
    fclose(f);

    if (!ok || !registry_valid(&loaded) || !registry_consistent(&loaded, NULL, NULL)) {
        fprintf(stderr, "%s is damaged or from another version, ignoring it\n", DATA_FILE);
        return STATUS_RECOVERY_ERROR;
    }
    *bank = loaded;
    return STATUS_OK;
}

status_t persistence_save(const bank_t *bank) {
    /* write a temporary file first so a crash cannot leave half a file */
    FILE *f = fopen(DATA_FILE_TMP, "wb");
    if (f == NULL) {
        perror("fopen " DATA_FILE_TMP);
        return STATUS_STORAGE_ERROR;
    }

    file_header_t header = { PROTOCOL_NUMBER, ACCOUNT_COUNT };
    int ok = fwrite(&header, sizeof header, 1, f) == 1 &&
             fwrite(bank, sizeof *bank, 1, f) == 1;
    if (fclose(f) != 0) {
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "could not write %s\n", DATA_FILE_TMP);
        DeleteFileA(DATA_FILE_TMP);
        return STATUS_STORAGE_ERROR;
    }

    if (!MoveFileExA(DATA_FILE_TMP, DATA_FILE, MOVEFILE_REPLACE_EXISTING)) {
        fprintf(stderr, "MoveFileEx failed: %lu\n", GetLastError());
        return STATUS_STORAGE_ERROR;
    }
    return STATUS_OK;
}
