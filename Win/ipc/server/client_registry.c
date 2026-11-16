/*
 * client_registry.c
 *
 * Manages the fixed-size table of bank accounts living inside shared memory.
 *
 * Design notes:
 * - No dynamic allocation: Everything sits inside a flat, contiguous bank_t
 *   struct so it can be mapped directly into Windows shared memory and dumped
 *   straight to disk as a raw binary file without pointer serialization.
 * - Lock-free internals: None of these functions acquire mutexes or semaphores.
 *   Callers must hold the transaction lock before calling them so that
 *   higher-level code can group multiple reads and writes into one atomic step
 *   (or intentionally skip locking when running --nolock race condition tests).
 */

#include "server.h"

#include <stdio.h>
#include <string.h>

/*
 * Builds a canonical 7-character account ID from a 0-based array index
 * (0 -> ACC0001, 1 -> ACC0002, etc.).
 *
 * Because client_id_t is written directly to disk and sent over named pipes,
 * we zero the entire struct before formatting. This guarantees that trailing
 * bytes after the null terminator never contain uninitialized stack garbage
 * that could leak over IPC or cause binary file diffs to look dirty.
 */
void registry_make_id(client_id_t *id, unsigned index) {
    if (id == NULL) {
        return;
    }

    memset(id, 0, sizeof(*id));
    snprintf(id->value, sizeof(id->value), "ACC%04u", index + 1U);
}

/*
 * Bootstraps a brand-new bank state from scratch when no saved data file exists
 * (or when starting fresh after corruption).
 *
 * Wiping the full bank_t struct clears out any leftover shared-memory bytes
 * from previous runs before seeding every account with the system's starting
 * capital. Each account starts at version 1 so transaction handlers can track
 * modifications over time.
 */
void registry_init(bank_t *bank) {
    if (bank == NULL) {
        return;
    }

    memset(bank, 0, sizeof(*bank));

    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        registry_make_id(&bank->accounts[i].client_id, i);

        bank->accounts[i].balance           = INITIAL_BALANCE;
        bank->accounts[i].total_deposits    = 0;
        bank->accounts[i].total_withdrawals = 0;
        bank->accounts[i].version           = 1;
        bank->accounts[i].flags             = 0;
    }
}

/*
 * Looks up an account by client ID using a straightforward linear scan.
 *
 * Since ACCOUNT_COUNT is small and fixed at compile time, scanning a contiguous
 * array is fast, CPU cache-friendly, and avoids the complexity of maintaining
 * hash buckets or tree pointers inside shared memory. We use strncmp bounded by
 * CLIENT_ID_LENGTH so a malformed, non-terminated ID from a client message can
 * never read past the buffer.
 */
account_t *registry_find(bank_t *bank, const client_id_t *id) {
    if (bank == NULL || id == NULL) {
        return NULL;
    }

    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        if (strncmp(bank->accounts[i].client_id.value, id->value, CLIENT_ID_LENGTH) == 0) {
            return &bank->accounts[i];
        }
    }

    return NULL;
}

/*
 * Sanity-checks a bank_t image right after loading it from utilities\bank.dat.
 *
 * Because persistence_load() reads raw bytes straight off disk into shared
 * memory, a truncated write, bit flip, or schema change (like altering
 * ACCOUNT_COUNT between builds) could leave garbage in memory. Verifying that
 * every slot holds its expected sequential ID and a non-negative balance
 * catches corrupted state before the server starts accepting clients.
 */
int registry_valid(const bank_t *bank) {
    if (bank == NULL) {
        return 0;
    }

    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        client_id_t expected;
        registry_make_id(&expected, i);

        if (strncmp(bank->accounts[i].client_id.value, expected.value, CLIENT_ID_LENGTH) != 0) {
            return 0;
        }
        if (bank->accounts[i].balance < 0) {
            return 0;
        }
    }

    return 1;
}

/*
 * Audits the entire bank against the fundamental conservation-of-money rule:
 *
 *   Sum of all current balances == Initial bank capital + All deposits - All withdrawals
 *
 * Why transfers are excluded: A transfer simply moves money from one account
 * to another inside the closed system, so the net effect on total bank funds
 * is zero.
 *
 * This check acts as our concurrency litmus test. Under normal locking, actual
 * and expected totals always match. When running the server with --nolock to
 * demonstrate race conditions, concurrent threads overwrite each other's balance
 * updates (lost updates), causing these two numbers to drift apart.
 *
 * Returns 1 if the books balance, or 0 if money was created or lost.
 */
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected) {
    if (bank == NULL) {
        return 0;
    }

    money_t sum  = 0;
    money_t want = (money_t)ACCOUNT_COUNT * INITIAL_BALANCE;

    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        sum  += bank->accounts[i].balance;
        want += bank->accounts[i].total_deposits;
        want -= bank->accounts[i].total_withdrawals;
    }

    /* Always report both figures if requested, even when the audit fails */
    if (actual != NULL) {
        *actual = sum;
    }
    if (expected != NULL) {
        *expected = want;
    }

    return sum == want;
}
