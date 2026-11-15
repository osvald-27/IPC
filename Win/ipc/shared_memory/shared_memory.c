#include "shared_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

struct shared_memory {
    HANDLE mapping;
    bank_t *bank;
};

static status_t map_view(HANDLE mapping, DWORD access, shared_memory_t **shm) {
    bank_t *view = MapViewOfFile(mapping, access, 0, 0, sizeof(bank_t));
    if (view == NULL) {
        fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return STATUS_SHARED_MEMORY_ERROR;
    }

    shared_memory_t *s = malloc(sizeof *s);
    if (s == NULL) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return STATUS_OUT_OF_MEMORY;
    }
    s->mapping = mapping;
    s->bank = view;
    *shm = s;
    return STATUS_OK;
}

status_t shared_memory_create(shared_memory_t **shm) {
    if (shm == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    /* INVALID_HANDLE_VALUE: no file on disk, the page file backs the memory */
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, (DWORD)sizeof(bank_t), SHM_NAME);
    if (mapping == NULL) {
        fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
        return STATUS_SHARED_MEMORY_ERROR;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* another server still owns the bank */
        CloseHandle(mapping);
        return STATUS_ALREADY_EXISTS;
    }
    return map_view(mapping, FILE_MAP_ALL_ACCESS, shm);
}

status_t shared_memory_open(shared_memory_t **shm, int read_only) {
    if (shm == NULL) {
        return STATUS_INVALID_ARGUMENT;
    }

    DWORD access = read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    HANDLE mapping = OpenFileMappingA(access, FALSE, SHM_NAME);
    if (mapping == NULL) {
        DWORD err = GetLastError();
        fprintf(stderr, "OpenFileMapping failed: %lu\n", err);
        return err == ERROR_FILE_NOT_FOUND ? STATUS_NOT_FOUND : STATUS_SHARED_MEMORY_ERROR;
    }
    return map_view(mapping, access, shm);
}

bank_t *shared_memory_bank(shared_memory_t *shm) {
    return shm == NULL ? NULL : shm->bank;
}

void shared_memory_close(shared_memory_t *shm) {
    if (shm == NULL) {
        return;
    }
    if (!UnmapViewOfFile(shm->bank)) {
        fprintf(stderr, "UnmapViewOfFile failed: %lu\n", GetLastError());
    }
    CloseHandle(shm->mapping);
    free(shm);
}
