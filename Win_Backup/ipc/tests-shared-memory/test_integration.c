/* test_integration.c - my modules used exactly the way the group's Windows server uses them:
 *   - the group's own types (bank_t, account_t, money_t) from common/protocol.h
 *   - worker THREADS (CreateThread), as in server/main.c
 *   - the group's lock interface: semaphore_create/open/lock/unlock/remove
 *   - shared memory created through shm_create
 * Exit code 0 = every PASS check succeeded. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../server/server.h"
#include "../shared_memory/shared_memory.h"
#include "../synchronization/semaphore.h"

#define I_SHM  "Local\\CrossIPC_IntTest_Shm"     /* test-only names: never touch a real server */
#define I_SEM  "Local\\CrossIPC_IntTest_Sem"

typedef struct {
    bank_t             *bank;
    transaction_lock_t *lock;
    int                 use_lock;
    int                 id, iters;
    HANDLE              gate;
    int                 failed;
} Worker;

static int g_fail = 0;
static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

static money_t total_of(const bank_t *b)
{
    money_t s = 0;
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) s += b->accounts[i].balance;
    return s;
}

static void widen_window(void)
{
    for (int k = 0; k < 200; k++) YieldProcessor();
    SwitchToThread();
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    Worker *w = (Worker *)arg;
    WaitForSingleObject(w->gate, 30000);                    /* all threads start together */
    unsigned seed = 4321u + (unsigned)w->id;
    for (int i = 0; i < w->iters; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned from = (seed >> 8) % ACCOUNT_COUNT;
        unsigned to = (from + 1 + ((seed >> 16) % (ACCOUNT_COUNT - 1))) % ACCOUNT_COUNT;
        money_t amount = 1 + (money_t)((seed >> 4) % 5000);

        if (w->use_lock && semaphore_lock(w->lock) != STATUS_OK) { w->failed++; continue; }

        account_t *a = &w->bank->accounts[from];
        account_t *b = &w->bank->accounts[to];
        if (a->balance >= amount) {
            money_t tmp = a->balance; widen_window(); a->balance = tmp - amount;
            tmp = b->balance;         widen_window(); b->balance = tmp + amount;
            a->version++; b->version++;
        }

        if (w->use_lock && semaphore_unlock(w->lock) != STATUS_OK) w->failed++;
    }
    return 0;
}

static int run(int use_lock, int threads, int iters, money_t *total_out, int *lock_errors)
{
    ShmRegion r;
    transaction_lock_t *lock = NULL;
    if (shm_create(I_SHM, sizeof(bank_t), &r) != SHM_OK) return -1;
    if (semaphore_create(I_SEM, &lock) != STATUS_OK) { shm_close(&r); return -1; }

    bank_t *bank = (bank_t *)r.view;
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) bank->accounts[i].balance = INITIAL_BALANCE;

    HANDLE gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    Worker w[8]; HANDLE h[8];
    for (int i = 0; i < threads; i++) {
        w[i] = (Worker){ bank, lock, use_lock, i + 1, iters, gate, 0 };
        h[i] = CreateThread(NULL, 0, worker_main, &w[i], 0, NULL);
    }
    Sleep(300);
    SetEvent(gate);
    WaitForMultipleObjects((DWORD)threads, h, TRUE, 120000);

    *lock_errors = 0;
    for (int i = 0; i < threads; i++) { *lock_errors += w[i].failed; CloseHandle(h[i]); }
    *total_out = total_of(bank);
    CloseHandle(gate);
    semaphore_remove(lock);
    shm_close(&r);
    return 0;
}

int main(void)
{
    const money_t expected = (money_t)ACCOUNT_COUNT * INITIAL_BALANCE;
    money_t total; int errs;

    printf("CrossIPC Windows: integration with the group's types and thread workers\n");
    printf("  sizeof(bank_t) = %u bytes, %u accounts, expected total = %lld\n",
           (unsigned)sizeof(bank_t), (unsigned)ACCOUNT_COUNT, (long long)expected);

    printf("\nTest A: the group's bank_t, 4 worker THREADS x 5000 transfers, WITH the lock\n");
    int rc = run(1, 4, 5000, &total, &errs);
    printf("  total = %lld\n", (long long)total);
    check(rc == 0, "shm_create + semaphore_create worked with the group's structures");
    check(errs == 0, "no lock or unlock call failed");
    check(total == expected, "total unchanged: no lost update");

    printf("\nTest B: same load WITHOUT the lock (the server's --nolock mode)\n");
    rc = run(0, 4, 5000, &total, &errs);
    printf("  total = %lld (expected %lld)\n", (long long)total, (long long)expected);
    printf("  [INFO] %s\n", total != expected ? "race condition observed" : "no corruption this time; run again");

    printf("\nTest C: a second server cannot take over live memory\n");
    ShmRegion a, b;
    check(shm_create(I_SHM, sizeof(bank_t), &a) == SHM_OK, "first shm_create succeeds");
    check(shm_create(I_SHM, sizeof(bank_t), &b) == SHM_EXISTS, "second shm_create returns SHM_EXISTS, so balances are not wiped");
    shm_close(&a);

    printf("\n%s (%d failed check%s)\n", g_fail ? "RESULT: FAILED" : "RESULT: ALL CHECKS PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
