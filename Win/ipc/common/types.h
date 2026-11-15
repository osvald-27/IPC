#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

#define ACCOUNT_COUNT 3
#define INITIAL_BALANCE 100000LL
#define TOTAL_EXPECTED (ACCOUNT_COUNT * INITIAL_BALANCE)

typedef int64_t amount_t;
typedef int32_t account_id_t;

typedef struct {
    account_id_t id;
    amount_t balance;
} Account;

typedef struct {
    Account accounts[ACCOUNT_COUNT];
} Bank;

#endif
