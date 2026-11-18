#include "server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* File header for bank.dat */
typedef struct {
    uint32_t magic;
    uint32_t account_count;
    uint64_t saved_at;
} file_header_t;

status_t persistence_load(bank_t *bank) {
    if (bank == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    FILE *f = fopen(DATA_FILE, "rb");
    if (f == NULL) {
        f = fopen("bank.dat", "rb");
        if (f == NULL) {
            return STATUS_NOT_FOUND;  /* First run, no saved state yet */
        }
    }

    file_header_t header;
    bank_t loaded;

    int ok = (fread(&header, sizeof(header), 1, f) == 1) &&
             (header.magic == PROTOCOL_NUMBER) &&
             (header.account_count == ACCOUNT_COUNT) &&
             (fread(&loaded, sizeof(loaded), 1, f) == 1);
    fclose(f);

    if (!ok || !registry_valid(&loaded) || !registry_consistent(&loaded, NULL, NULL)) {
        fprintf(stderr, "persistence_load: file damaged or incompatible, rejecting it\n");
        return STATUS_RECOVERY_ERROR;
    }

    *bank = loaded;
    return STATUS_OK;
}

status_t persistence_save(const bank_t *bank) {
    if (bank == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    const char *tmp_path = DATA_FILE_TMP;
    const char *final_path = DATA_FILE;

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        /* Fallback if utilities directory is inaccessible */
        tmp_path = "bank.dat.tmp";
        final_path = "bank.dat";
        f = fopen(tmp_path, "wb");
        if (f == NULL) {
            perror("persistence_save: fopen tmp");
            return STATUS_STORAGE_ERROR;
        }
    }

    file_header_t header;
    header.magic = PROTOCOL_NUMBER;
    header.account_count = ACCOUNT_COUNT;
    header.saved_at = (uint64_t)time(NULL);

    int ok = (fwrite(&header, sizeof(header), 1, f) == 1) &&
             (fwrite(bank, sizeof(*bank), 1, f) == 1);

    if (fflush(f) != 0) {
        ok = 0;
    }
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
    if (fclose(f) != 0) {
        ok = 0;
    }

    if (!ok) {
        fprintf(stderr, "persistence_save: failed to write complete state\n");
        unlink(tmp_path);
        return STATUS_STORAGE_ERROR;
    }

    /* Atomic rename replaces existing file safely */
    if (rename(tmp_path, final_path) != 0) {
        perror("persistence_save: rename");
        unlink(tmp_path);
        return STATUS_STORAGE_ERROR;
    }

    return STATUS_OK;
}

/* Write-Ahead Log: Appends every completed or rejected transaction to the log */
status_t persistence_log_transaction(const transaction_t *tx) {
    if (tx == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    const char *log_path = TRANSACTIONS_LOG;
    FILE *f = fopen(log_path, "a");
    if (f == NULL) {
        log_path = "transactions.log";
        f = fopen(log_path, "a");
        if (f == NULL) {
            return STATUS_STORAGE_ERROR;
        }
    }

    const char *type_str = "UNKNOWN";
    if (tx->type == TRANSACTION_DEPOSIT) type_str = "DEPOSIT";
    else if (tx->type == TRANSACTION_WITHDRAWAL) type_str = "WITHDRAW";
    else if (tx->type == TRANSACTION_TRANSFER) type_str = "TRANSFER";

    const char *state_str = "UNKNOWN";
    if (tx->state == TRANSACTION_COMMITTED) state_str = "COMMITTED";
    else if (tx->state == TRANSACTION_REJECTED) state_str = "REJECTED";
    else if (tx->state == TRANSACTION_FAILED) state_str = "FAILED";
    else if (tx->state == TRANSACTION_PENDING) state_str = "PENDING";

    fprintf(f, "%" PRIu64 " | TXN=%s | REQ=%s | TYPE=%s | SENDER=%s | RECV=%s | AMT=%" PRId64 " | STATE=%s | STATUS=%s (%d)\n",
            tx->completed_at ? tx->completed_at : (uint64_t)time(NULL),
            tx->transaction_id.value,
            tx->request_id.value,
            type_str,
            tx->sender_id.value[0] ? tx->sender_id.value : "-",
            tx->receiver_id.value[0] ? tx->receiver_id.value : "-",
            tx->amount,
            state_str,
            server_status_string(tx->result),
            tx->result);

    fflush(f);
    fclose(f);
    return STATUS_OK;
}
