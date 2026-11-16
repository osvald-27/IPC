/* txlock.c - named-mutex implementation of the transaction lock (Windows). */
#include <windows.h>
#include <stdio.h>
#include "txlock.h"

static void report(const char *fn, const char *name)
{
    fprintf(stderr, "txlock: %s(\"%s\") failed, error %lu\n", fn, name ? name : "", GetLastError());
}

TxLockStatus txlock_create(const char *name, TxLock *out)
{
    out->handle = NULL;
    HANDLE h = CreateMutexA(NULL, FALSE, name);        /* FALSE: nobody owns it at start */
    if (h == NULL) { report("CreateMutex", name); return TXLOCK_ERROR; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {      /* the name existed before this call */
        CloseHandle(h);
        return TXLOCK_EXISTS;
    }
    out->handle = h;
    return TXLOCK_OK;
}

TxLockStatus txlock_open(const char *name, TxLock *out)
{
    out->handle = NULL;
    HANDLE h = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name);
    if (h == NULL) {
        DWORD e = GetLastError();                      /* read it BEFORE printing anything */
        report("OpenMutex", name);
        SetLastError(e);
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return TXLOCK_NOT_FOUND;
        if (e == ERROR_ACCESS_DENIED) return TXLOCK_DENIED;
        return TXLOCK_ERROR;
    }
    out->handle = h;
    return TXLOCK_OK;
}

TxLockStatus txlock_acquire(TxLock *l, uint32_t timeout_ms)
{
    if (l == NULL || l->handle == NULL) return TXLOCK_ERROR;
    DWORD r = WaitForSingleObject(l->handle, timeout_ms);
    switch (r) {
    case WAIT_OBJECT_0: return TXLOCK_OK;
    case WAIT_ABANDONED: return TXLOCK_ABANDONED;      /* we own it now; previous owner died */
    case WAIT_TIMEOUT:  return TXLOCK_TIMEOUT;
    default:
        fprintf(stderr, "txlock: WaitForSingleObject returned %lu, error %lu\n", r, GetLastError());
        return TXLOCK_ERROR;
    }
}

TxLockStatus txlock_release(TxLock *l)
{
    if (l == NULL || l->handle == NULL) return TXLOCK_ERROR;
    if (!ReleaseMutex(l->handle)) {                    /* ERROR_NOT_OWNER if we never locked it */
        report("ReleaseMutex", NULL);
        return TXLOCK_ERROR;
    }
    return TXLOCK_OK;
}

void txlock_close(TxLock *l)
{
    if (l && l->handle) {
        if (!CloseHandle(l->handle)) report("CloseHandle", NULL);
        l->handle = NULL;
    }
}
