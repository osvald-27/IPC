#include "server.h"

#include <stdio.h>
#include <string.h>

/* Account ids are ACC0001, ACC0002, ... which fits CLIENT_ID_LENGTH (7). */
void registry_make_id(client_id_t *id, unsigned index) {
    memset(id, 0, sizeof *id);
    snprintf(id->value, sizeof id->value, "ACC%04u", index + 1U);
}

void registry_init(bank_t *bank) {
    memset(bank, 0, sizeof *bank);
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        registry_make_id(&bank->accounts[i].client_id, i);
        bank->accounts[i].balance = INITIAL_BALANCE;
    }
}

account_t *registry_find(bank_t *bank, const client_id_t *id) {
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        if (strncmp(bank->accounts[i].client_id.value, id->value, CLIENT_ID_LENGTH) == 0) {
            return &bank->accounts[i];
        }
    }
    return NULL;
}

/* Used after loading from disk: the file must hold the accounts we expect. */
int registry_valid(const bank_t *bank) {
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
 * The consistency check from the brief.
 * Transfers only move money, so:
 *   sum of balances = initial total + all deposits - all withdrawals
 * Call it with the lock held, otherwise a transfer in progress can be
 * seen half done.
 */
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected) {
    money_t sum = 0;
    money_t want = (money_t)ACCOUNT_COUNT * INITIAL_BALANCE;

    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        sum += bank->accounts[i].balance;
        want += bank->accounts[i].total_deposits;
        want -= bank->accounts[i].total_withdrawals;
    }
    if (actual != NULL) {
        *actual = sum;
    }
    if (expected != NULL) {
        *expected = want;
    }
    return sum == want;
}
