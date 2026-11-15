#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stdint.h>

#include "types.h"
#include "status.h"

/* Explicit numbers to keep the wire and shared-memory layout stable
 * across different compilers, while NONE, that is (0) catches
 *  uninitialized structs before the server processes them. This
 * Structure allows for the distinguishing of incoming request
 * type allowing for it to met with the correct service.
 */
typedef enum {
    TRANSACTION_NONE       = 0,
    TRANSACTION_DEPOSIT    = 1,
    TRANSACTION_WITHDRAWAL = 2,
    TRANSACTION_TRANSFER   = 3
} transaction_type_t;

/* Tracks the full lifecycle of a transaction from creation to completion. 
 * This allows for clients to safely polling on request.
 */
typedef enum {
    TRANSACTION_CREATED    = 0,
    TRANSACTION_PENDING    = 1,
    TRANSACTION_PROCESSING = 2,
    TRANSACTION_COMMITTED  = 3,
    TRANSACTION_REJECTED   = 4,
    TRANSACTION_FAILED     = 5
} transaction_state_t;

/*
 * Once submitted by a client, the core details (IDs, type, amount) should
 * stay fixed. Only the server updates state, result, and completed_at
 * so there is no race condition over transaction terms.
 */
typedef struct {
    transaction_id_t transaction_id;
    request_id_t request_id;

    transaction_type_t type;

    client_id_t sender_id;
    client_id_t receiver_id;

    money_t amount;

    transaction_state_t state;
    status_t result;

    uint64_t created_at;
    uint64_t completed_at;
} transaction_t;

/*
 * Tracks running totals for a single account separately from
 * live balance so audits and reconciliation stay straightforward.
 */
typedef struct {
    money_t total_deposits;
    money_t total_withdrawals;
    money_t total_transfers_in;
    money_t total_transfers_out;
} account_ledger_t;

#endif /* TRANSACTION_H */
