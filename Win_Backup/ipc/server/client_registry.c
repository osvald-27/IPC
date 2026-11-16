#include "server.h"

#include <stdio.h>
#include <string.h>

/* Formats account IDs as ACC0001, ACC0002, ACC0003 (fits CLIENT_ID_LENGTH = 7) */
void registry_make_id(client_id_t *id, unsigned index) {
    if (id == NULL) {
        return;
    }
    memset(id, 0, sizeof(*id));
    snprintf(id->value, sizeof(id->value), "ACC%04u", index + 1U);
}

void registry_init(bank_t *bank) {
    if (bank == NULL) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        registry_make_id(&bank->accounts[i].client_id, i);
        bank->accounts[i].balance = INITIAL_BALANCE;
        bank->accounts[i].total_deposits = 0;
        bank->accounts[i].total_withdrawals = 0;
        bank->accounts[i].version = 1;
        bank->accounts[i].flags = 0;
    }
}

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

/* Validates that loaded accounts match the system layout and have non-negative balances */
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
 * Verifies the accounting invariant from the specification:
 * Total bank balance must equal:
 *   initial total + all deposits - all withdrawals
 * Transfers preserve the total since money is only shifted between accounts.
 */
int registry_consistent(const bank_t *bank, money_t *actual, money_t *expected) {
    if (bank == NULL) {
        return 0;
    }

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
