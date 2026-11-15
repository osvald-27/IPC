#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "types.h"
#include "status.h"
#include "transaction.h"

#define ACCOUNT_COUNT    3U
#define INITIAL_BALANCE  100000LL   /* XAF per account */

/* Windows object names */
#define PIPE_NAME   "\\\\.\\pipe\\BankPipe"
#define SHM_NAME    "Local\\BankSharedMemory"
#define SEM_NAME    "Local\\BankSemaphore"

typedef struct {
    request_id_t  request_id;
    session_id_t  session_id;
    transaction_t transaction;
} request_t;

typedef struct {
    request_id_t request_id;
    status_t     status;
    money_t      balance;   /* balance after the operation */
} reply_t;

typedef struct {
    account_t accounts[ACCOUNT_COUNT];
} bank_t;   /* this is what lives in shared memory */

#endif /* PROTOCOL_H */
