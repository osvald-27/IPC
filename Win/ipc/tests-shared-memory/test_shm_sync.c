/* test_shm_sync.c - verification of shared_memory + semaphore (Windows).
 * Run with no arguments. The same exe starts itself as child processes
 * (CreateProcess) to play the roles of workers, a crashing worker and a client.
 * Exit code 0 = every PASS check succeeded. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../shared_memory/shared_memory.h"
#include "../synchronization/txlock.h"
#include "../synchronization/semaphore.h"   /* the group-facing interface */

/* Test-only names, so a real server running at the same time is not disturbed. */
#define T_AUTH "Local\\CrossIPC_Test_Auth"
#define T_SNAP "Local\\CrossIPC_Test_Snap"
#define T_LOCK "Local\\CrossIPC_Test_Lock"
#define T_GO   "Local\\CrossIPC_Test_Go"     /* start gate: all workers begin together */

#define NACC 3
#define INITIAL_TOTAL 300000LL
#define SNAP_FINAL 200000u

/* Minimal stand-in for the authoritative region, with a journal for crash recovery. */
typedef struct {
    int64_t  balance[NACC];
    uint32_t in_flight;          /* 1 while a transfer is half applied */
    int32_t  from, to;
    int64_t  old_from, old_to;   /* balances before the transfer: the "undo" information */
} TestAuth;

typedef struct { uint64_t a, b, c; } TestSnap;   /* invariant: b == 2*a and c == a+1 */

static int g_fail = 0;
static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static int64_t total_of(const volatile TestAuth *t)
{
    int64_t s = 0;
    for (int i = 0; i < NACC; i++) s += t->balance[i];
    return s;
}

static void init_auth(volatile TestAuth *t)
{
    t->balance[0] = 100000; t->balance[1] = 150000; t->balance[2] = 50000;
    t->in_flight = 0;
}

/* Roll back a half-applied transfer. Called by whoever receives TXLOCK_ABANDONED. */
static void recover(volatile TestAuth *t)
{
    if (t->in_flight) {
        t->balance[t->from] = t->old_from;
        t->balance[t->to]   = t->old_to;
        t->in_flight = 0;
    }
}

/* Pause between reading a balance and writing it back. A real race window is only a few
 * nanoseconds; this makes it long enough that a missing lock is detected reliably. */
static void widen_window(void)
{
    for (int k = 0; k < 200; k++) YieldProcessor();
    SwitchToThread();
}

/* One transfer: read, pause, write. */
static int transfer(volatile TestAuth *t, TxLock *lock, int use_lock, int from, int to, int64_t amt)
{
    if (use_lock) {
        TxLockStatus rc = txlock_acquire(lock, 10000);
        if (rc == TXLOCK_ABANDONED) recover(t);
        else if (rc != TXLOCK_OK) return -1;
    }
    if (t->balance[from] >= amt) {
        t->old_from = t->balance[from]; t->old_to = t->balance[to];
        t->from = from; t->to = to; t->in_flight = 1;
        int64_t tmp = t->balance[from]; widen_window(); t->balance[from] = tmp - amt;
        tmp = t->balance[to];           widen_window(); t->balance[to]   = tmp + amt;
        t->in_flight = 0;
    }
    if (use_lock) txlock_release(lock);
    return 0;
}

/* ------------------------------ helpers for processes ------------------------------ */

static HANDLE spawn(const char *args)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 128];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "CreateProcess failed: error %lu\n", GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* Wait for a child and return its exit code (0xFFFFDEAD if it had to be killed). */
static DWORD wait_exit(HANDLE h, DWORD timeout_ms)
{
    DWORD code = 0xFFFFDEADu;
    if (h == NULL) return code;
    if (WaitForSingleObject(h, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(h, &code);
    else TerminateProcess(h, 0xFFFFDEADu);
    CloseHandle(h);
    return code;
}

/* ---------------------------------- tests ---------------------------------- */

static void test_lifecycle(void)
{
    printf("\nTest 1: creation, duplicate names\n");
    ShmRegion a, b; TxLock l1, l2;
    check(shm_create(T_AUTH, sizeof(TestAuth), &a) == SHM_OK, "shm_create succeeds");
    check(shm_create(T_AUTH, sizeof(TestAuth), &b) == SHM_EXISTS, "second shm_create reports SHM_EXISTS (no silent reuse)");
    check(txlock_create(T_LOCK, &l1) == TXLOCK_OK, "txlock_create succeeds");
    check(txlock_create(T_LOCK, &l2) == TXLOCK_EXISTS, "second txlock_create reports TXLOCK_EXISTS");
    txlock_close(&l1); shm_close(&a);
    shm_close(&a);                                   /* second close must be harmless */
    check(1, "closing twice does not crash");
}

static int run_concurrent(int use_lock, int workers, int iters, int64_t *total_out)
{
    ShmRegion r; TxLock lock;
    if (shm_create(T_AUTH, sizeof(TestAuth), &r) != SHM_OK) return -1;
    if (txlock_create(T_LOCK, &lock) != TXLOCK_OK) { shm_close(&r); return -1; }
    volatile TestAuth *t = (volatile TestAuth *)r.view;
    init_auth(t);

    HANDLE go = CreateEventA(NULL, TRUE, FALSE, T_GO);   /* manual-reset start gate, closed */
    if (go == NULL) { txlock_close(&lock); shm_close(&r); return -1; }

    HANDLE h[16]; char args[96];
    for (int i = 0; i < workers; i++) {
        snprintf(args, sizeof args, "--worker %d %d %d", i + 1, iters, use_lock);
        h[i] = spawn(args);
    }
    Sleep(1000);          /* let every worker reach the gate */
    SetEvent(go);         /* ... then release them all at the same instant */
    int bad = 0;
    for (int i = 0; i < workers; i++) if (wait_exit(h[i], 120000) != 0) bad++;
    *total_out = total_of(t);
    int in_flight = (int)t->in_flight;
    CloseHandle(go);
    txlock_close(&lock); shm_close(&r);
    return bad ? -1 : in_flight;                     /* in_flight is 0 on a clean run */
}

static void test_concurrent_locked(void)
{
    printf("\nTest 2: 4 worker processes, 5000 transfers each, WITH the lock\n");
    int64_t total = 0;
    int rc = run_concurrent(1, 4, 5000, &total);
    printf("  total = %lld (expected %lld)\n", (long long)total, (long long)INITIAL_TOTAL);
    check(rc == 0, "all workers finished, nothing left half-applied");
    check(total == INITIAL_TOTAL, "total unchanged: no lost update");
}

static void test_concurrent_unlocked(void)
{
    printf("\nTest 3: same workload WITHOUT the lock (expected to fail: demonstrates the race)\n");
    int64_t total = 0;
    int rc = run_concurrent(0, 4, 5000, &total);
    (void)rc;
    printf("  total = %lld (expected %lld)\n", (long long)total, (long long)INITIAL_TOTAL);
    printf("  [INFO] %s\n", total != INITIAL_TOTAL ? "race condition observed: money was created or destroyed"
                                                    : "no corruption this time; run again (races are probabilistic)");
}

static void test_crash_recovery(void)
{
    printf("\nTest 4: a worker dies while holding the lock\n");
    ShmRegion r; TxLock lock;
    if (shm_create(T_AUTH, sizeof(TestAuth), &r) != SHM_OK || txlock_create(T_LOCK, &lock) != TXLOCK_OK) {
        check(0, "setup"); return;
    }
    volatile TestAuth *t = (volatile TestAuth *)r.view;
    init_auth(t);

    DWORD code = wait_exit(spawn("--crasher"), 20000);
    check(code == 99, "crasher process was killed in the middle of a transfer (exit code 99)");
    printf("  total after the crash = %lld (money is missing)\n", (long long)total_of(t));
    check(total_of(t) != INITIAL_TOTAL && t->in_flight == 1, "data is inconsistent and the journal says a transfer was in flight");

    TxLockStatus rc = txlock_acquire(&lock, 5000);
    check(rc == TXLOCK_ABANDONED, "next process is told the owner died (TXLOCK_ABANDONED), not blocked forever");
    if (rc == TXLOCK_ABANDONED || rc == TXLOCK_OK) {
        recover(t);
        txlock_release(&lock);
    }
    check(total_of(t) == INITIAL_TOTAL && t->in_flight == 0, "rollback from the journal restored the total");
    check(txlock_acquire(&lock, 1000) == TXLOCK_OK, "afterwards the lock behaves normally (TXLOCK_OK)");
    txlock_release(&lock);
    txlock_close(&lock); shm_close(&r);
}

static void test_timeout(void)
{
    printf("\nTest 5: timeout instead of waiting forever\n");
    ShmRegion r; TxLock lock;
    if (shm_create(T_AUTH, sizeof(TestAuth), &r) != SHM_OK || txlock_create(T_LOCK, &lock) != TXLOCK_OK) {
        check(0, "setup"); return;
    }
    check(txlock_acquire(&lock, 1000) == TXLOCK_OK, "parent takes the lock");
    DWORD code = wait_exit(spawn("--probe-timeout"), 20000);
    check(code == 0, "a second process gets TXLOCK_TIMEOUT after 300 ms while the lock is held");
    txlock_release(&lock);
    printf("  (the 'error 288' line below is EXPECTED: 288 = ERROR_NOT_OWNER)\n");
    check(txlock_release(&lock) == TXLOCK_ERROR, "releasing a lock we do not own is detected (not owner)");
    txlock_close(&lock); shm_close(&r);
}

static void test_snapshot(void)
{
    printf("\nTest 6: snapshot read by another process while the server rewrites it\n");
    ShmRegion r;
    if (shm_create(T_SNAP, snap_region_size(sizeof(TestSnap)), &r) != SHM_OK) { check(0, "setup"); return; }
    HANDLE reader = spawn("--reader");
    Sleep(200);
    TestSnap s;
    for (uint32_t i = 1; i <= SNAP_FINAL; i++) {
        s.a = i; s.b = 2ull * i; s.c = i + 1ull;
        if (snap_publish(&r, &s, sizeof s) != SNAP_OK) break;
    }
    DWORD code = wait_exit(reader, 60000);
    check(code == 0, "reader saw only complete snapshots (no torn read) and reached the final value");
    shm_close(&r);
}

static void test_read_only(void)
{
    printf("\nTest 7: a client cannot write through its read-only mapping\n");
    ShmRegion r;
    if (shm_create(T_SNAP, snap_region_size(sizeof(TestSnap)), &r) != SHM_OK) { check(0, "setup"); return; }
    DWORD code = wait_exit(spawn("--ro-writer"), 20000);
    printf("  child exit code = 0x%08lX\n", code);
    check(code == 0xC0000005u, "write through shm_open_ro region -> access violation (0xC0000005)");
    shm_close(&r);
}

static void test_cleanup(void)
{
    printf("\nTest 8: nothing left behind after every handle is closed\n");
    printf("  (the two 'failed, error 2' lines below are EXPECTED: error 2 = object not found)\n");
    ShmRegion r; TxLock l;
    check(shm_open_rw(T_AUTH, sizeof(TestAuth), &r) == SHM_NOT_FOUND, "shared-memory object no longer exists (SHM_NOT_FOUND)");
    check(txlock_open(T_LOCK, &l) == TXLOCK_NOT_FOUND, "lock object no longer exists (TXLOCK_NOT_FOUND)");
}


static void test_group_interface(void)
{
    printf("\nTest 9: the group's semaphore_* interface (what server/transaction.c calls)\n");
    transaction_lock_t *a = NULL, *b = NULL, *c = NULL;
    check(semaphore_create(T_LOCK, &a) == STATUS_OK, "semaphore_create returns STATUS_OK");
    check(semaphore_create(T_LOCK, &c) == STATUS_ALREADY_EXISTS, "second semaphore_create returns STATUS_ALREADY_EXISTS");
    check(semaphore_open(T_LOCK, &b) == STATUS_OK, "semaphore_open attaches to it (as main.c does on ALREADY_EXISTS)");
    check(semaphore_lock(a) == STATUS_OK, "semaphore_lock returns STATUS_OK");
    check(semaphore_unlock(a) == STATUS_OK, "semaphore_unlock returns STATUS_OK");
    printf("  (the 'error 288' line below is EXPECTED)\n");
    check(semaphore_unlock(a) == STATUS_SYNCHRONIZATION_ERROR, "unlock without owning -> STATUS_SYNCHRONIZATION_ERROR");
    check(semaphore_lock(NULL) == STATUS_INVALID_ARGUMENT, "NULL handle -> STATUS_INVALID_ARGUMENT");
    semaphore_close(b);
    check(semaphore_remove(a) == STATUS_OK, "semaphore_remove returns STATUS_OK");
    transaction_lock_t *d = NULL;
    printf("  (the 'error 2' line below is EXPECTED)\n");
    check(semaphore_open(T_LOCK, &d) == STATUS_NOT_FOUND, "after remove, open -> STATUS_NOT_FOUND");
}

/* ------------------------------ child roles ------------------------------ */

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    (void)ep;
    return EXCEPTION_EXECUTE_HANDLER;   /* terminate silently; exit code = exception code */
}

static int child_worker(int id, int iters, int use_lock)
{
    ShmRegion r; TxLock lock = {0};
    if (shm_open_rw(T_AUTH, sizeof(TestAuth), &r) != SHM_OK) return 10;
    if (use_lock && txlock_open(T_LOCK, &lock) != TXLOCK_OK) return 11;
    volatile TestAuth *t = (volatile TestAuth *)r.view;
    HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, T_GO);
    if (go == NULL) return 13;
    if (WaitForSingleObject(go, 30000) != WAIT_OBJECT_0) return 14;
    CloseHandle(go);
    srand(1234u + (unsigned)id);
    for (int i = 0; i < iters; i++) {
        int from = rand() % NACC;
        int to = (from + 1 + rand() % (NACC - 1)) % NACC;
        if (transfer(t, &lock, use_lock, from, to, 1 + rand() % 5000) != 0) return 12;
    }
    if (use_lock) txlock_close(&lock);
    shm_close(&r);
    return 0;
}

static int child_crasher(void)
{
    ShmRegion r; TxLock lock;
    if (shm_open_rw(T_AUTH, sizeof(TestAuth), &r) != SHM_OK) return 10;
    if (txlock_open(T_LOCK, &lock) != TXLOCK_OK) return 11;
    volatile TestAuth *t = (volatile TestAuth *)r.view;
    if (txlock_acquire(&lock, 5000) != TXLOCK_OK) return 12;
    t->old_from = t->balance[0]; t->old_to = t->balance[1];
    t->from = 0; t->to = 1; t->in_flight = 1;       /* journal first ... */
    t->balance[0] -= 7000;                          /* ... debit done, credit NOT done ... */
    TerminateProcess(GetCurrentProcess(), 99);      /* ... and the process dies holding the lock */
    return 13;
}

static int child_probe_timeout(void)
{
    TxLock lock;
    if (txlock_open(T_LOCK, &lock) != TXLOCK_OK) return 11;
    return txlock_acquire(&lock, 300) == TXLOCK_TIMEOUT ? 0 : 1;
}

static int child_reader(void)
{
    ShmRegion r;
    if (shm_open_ro(T_SNAP, snap_region_size(sizeof(TestSnap)), &r) != SHM_OK) return 10;
    DWORD start = GetTickCount();
    unsigned long reads = 0, torn = 0;
    TestSnap s;
    while (GetTickCount() - start < 30000) {
        SnapStatus st = snap_read(&r, &s, sizeof s, 1000);
        if (st == SNAP_EMPTY || st == SNAP_BUSY) { SwitchToThread(); continue; }
        if (st != SNAP_OK) return 12;
        reads++;
        if (s.b != 2 * s.a || s.c != s.a + 1) torn++;
        if (s.a == SNAP_FINAL) break;
    }
    printf("  reader: %lu consistent reads, %lu torn reads\n", reads, torn);
    shm_close(&r);
    if (torn) return 1;
    return reads ? 0 : 2;
}

static int child_ro_writer(void)
{
    ShmRegion r;
    if (shm_open_ro(T_SNAP, snap_region_size(sizeof(TestSnap)), &r) != SHM_OK) return 10;
    *(volatile unsigned char *)r.view = 0x41;       /* illegal: the view is read-only */
    return 7;                                       /* only reached if the write was allowed */
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(crash_filter);

    if (argc >= 2) {
        if (!strcmp(argv[1], "--worker") && argc == 5) return child_worker(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        if (!strcmp(argv[1], "--crasher"))         return child_crasher();
        if (!strcmp(argv[1], "--probe-timeout"))   return child_probe_timeout();
        if (!strcmp(argv[1], "--reader"))          return child_reader();
        if (!strcmp(argv[1], "--ro-writer"))       return child_ro_writer();
        fprintf(stderr, "unknown option\n");
        return 2;
    }

    printf("CrossIPC Windows: shared memory + lock verification\n");
    test_lifecycle();
    test_concurrent_locked();
    test_concurrent_unlocked();
    test_crash_recovery();
    test_timeout();
    test_snapshot();
    test_read_only();
    test_cleanup();
    test_group_interface();

    printf("\n%s (%d failed check%s)\n", g_fail ? "RESULT: FAILED" : "RESULT: ALL CHECKS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
