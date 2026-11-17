 /* TRANSACTION_H */
#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stdint.h>

#include "types.h"
#include "status.h"

typedef enum {
    TRANSACTION_DEPOSIT = 1,
    TRANSACTION_WITHDRAWAL = 2,
    TRANSACTION_TRANSFER = 3
} transaction_type_t;

typedef enum {
    TRANSACTION_PENDING = 0,
    TRANSACTION_COMMITTED = 1,
    TRANSACTION_REJECTED = 2,
    TRANSACTION_FAILED = 3
} transaction_state_t;

typedef struct {
    transaction_id_t transaction_id;
    request_id_t request_id;
    transaction_type_t type;
    transaction_state_t state;
    status_t result;
    client_id_t sender_id;
    client_id_t receiver_id;
    money_t amount;
    uint64_t created_at;
    uint64_t completed_at;
} transaction_t;

#endif
