<<<<<<< HEAD
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
=======
/* shared_memory.c - file-mapping implementation (Windows). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "shared_memory.h"

static void report(const char *fn, const char *name)
{
    fprintf(stderr, "shared_memory: %s(\"%s\") failed, error %lu\n", fn, name ? name : "", GetLastError());
}

ShmStatus shm_create(const char *name, size_t size, ShmRegion *out)
{
    memset(out, 0, sizeof *out);
    uint64_t sz = (uint64_t)size;
    /* INVALID_HANDLE_VALUE: backed by the paging file, not by a file we created.
       Windows zero-fills the pages. */
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                  (DWORD)(sz >> 32), (DWORD)(sz & 0xFFFFFFFFu), name);
    if (h == NULL) { report("CreateFileMapping", name); return SHM_ERROR; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {      /* name was already taken */
        CloseHandle(h);
        return SHM_EXISTS;
    }
    void *v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (v == NULL) { report("MapViewOfFile", name); CloseHandle(h); return SHM_ERROR; }
    out->handle = h; out->view = v; out->size = size; out->read_only = 0;
    return SHM_OK;
}

static ShmStatus open_region(const char *name, size_t size, ShmRegion *out, int read_only)
{
    memset(out, 0, sizeof *out);
    DWORD access = read_only ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    HANDLE h = OpenFileMappingA(access, FALSE, name);
    if (h == NULL) {
        DWORD e = GetLastError();                      /* read it BEFORE printing anything */
        report("OpenFileMapping", name);
        SetLastError(e);
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? SHM_NOT_FOUND : SHM_ERROR;
    }
    /* Asking for 'size' bytes also fails if the region is smaller than we expect. */
    void *v = MapViewOfFile(h, access, 0, 0, size);
    if (v == NULL) { report("MapViewOfFile", name); CloseHandle(h); return SHM_ERROR; }
    out->handle = h; out->view = v; out->size = size; out->read_only = read_only;
    return SHM_OK;
}

ShmStatus shm_open_rw(const char *name, size_t size, ShmRegion *out) { return open_region(name, size, out, 0); }
ShmStatus shm_open_ro(const char *name, size_t size, ShmRegion *out) { return open_region(name, size, out, 1); }

void shm_close(ShmRegion *r)
{
    if (r == NULL) return;
    if (r->view && !UnmapViewOfFile(r->view)) report("UnmapViewOfFile", NULL);
    if (r->handle && !CloseHandle(r->handle)) report("CloseHandle", NULL);
    memset(r, 0, sizeof *r);
}

/* ---------------------------- snapshot (seqlock) ---------------------------- */

#define SNAP_MAGIC 0x50414E53u   /* 'SNAP' */

typedef struct {
    uint32_t      magic;         /* 0 until the first publish */
    uint32_t      payload_size;
    volatile LONG sequence;      /* odd = writer busy, even = stable */
    uint32_t      reserved;
} SnapHeader;                    /* 16 bytes, payload follows */

size_t snap_region_size(size_t payload_size) { return sizeof(SnapHeader) + payload_size; }

SnapStatus snap_publish(ShmRegion *r, const void *payload, size_t len)
{
    if (r == NULL || r->view == NULL || r->read_only || payload == NULL ||
        snap_region_size(len) > r->size) {
        fprintf(stderr, "shared_memory: snap_publish: bad region or payload size\n");
        return SNAP_ERROR;
    }
    SnapHeader *h = (SnapHeader *)r->view;
    if (h->magic != SNAP_MAGIC) {          /* first publish: initialise the header */
        h->payload_size = (uint32_t)len;
        h->sequence = 0;
        MemoryBarrier();
        h->magic = SNAP_MAGIC;
    }
    InterlockedIncrement(&h->sequence);                  /* now odd: readers must wait/retry */
    memcpy((unsigned char *)r->view + sizeof(SnapHeader), payload, len);
    MemoryBarrier();
    InterlockedIncrement(&h->sequence);                  /* even again: new data is complete */
    return SNAP_OK;
}

SnapStatus snap_read(const ShmRegion *r, void *out, size_t len, int max_tries)
{
    if (r == NULL || r->view == NULL || out == NULL || snap_region_size(len) > r->size)
        return SNAP_ERROR;
    const SnapHeader *h = (const SnapHeader *)r->view;
    if (h->magic != SNAP_MAGIC) return SNAP_EMPTY;
    if (h->payload_size != len) return SNAP_ERROR;       /* reader and server disagree on layout */
    for (int i = 0; i < max_tries; i++) {
        LONG s1 = h->sequence;
        if (s1 & 1) { YieldProcessor(); continue; }      /* writer is busy */
        MemoryBarrier();
        memcpy(out, (const unsigned char *)r->view + sizeof(SnapHeader), len);
        MemoryBarrier();
        if (h->sequence == s1) return SNAP_OK;           /* nothing changed while we copied */
    }
    return SNAP_BUSY;
>>>>>>> origin/main
}
