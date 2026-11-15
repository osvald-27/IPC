#ifndef TRANSACTION_H
#define TRANSACTION_H

#include "types.h"

typedef enum {
    TRANSACTION_DEPOSIT = 1,
    TRANSACTION_WITHDRAW,
    TRANSACTION_TRANSFER
} transaction_type_t;

typedef struct {
    transaction_id_t   id;
    transaction_type_t type;
    client_id_t        from;    /* unused for a deposit */
    client_id_t        to;      /* unused for a withdrawal */
    money_t            amount;  /* whole XAF, must be > 0 */
} transaction_t;

#endif /* TRANSACTION_H */
