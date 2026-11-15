#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#include "types.h"
#include "status.h"
#include "transaction.h"

/* "CRIP" in ASCII: lets the server reject corrupted or
 * unrelated bytes before parsing the rest of the packet.
 */
#define PROTOCOL_NUMBER         0x43524950U
#define PROTOCOL_VERSION        1U

#define MAX_PAYLOAD_SIZE        4096U
#define MAX_MESSAGE_SIZE        8192U

typedef enum {
    REQUEST_NONE               = 0,

    REQUEST_INITIALIZE         = 1,
    REQUEST_LOGIN              = 2,
    REQUEST_LOGOUT             = 3,

    REQUEST_GET_BALANCE        = 10,

    REQUEST_DEPOSIT            = 20,
    REQUEST_WITHDRAWAL         = 21,
    REQUEST_TRANSFER           = 22,

    REQUEST_TRANSACTION_STATUS = 30,

    REQUEST_SERVER_STATUS      = 40,

    RESPONSE_MESSAGE           = 100
} message_type_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint32_t message_type;
    uint32_t payload_size;

    request_id_t request_id;
    session_id_t session_id;
} message_header_t;

typedef struct {
    client_id_t client_id;
} login_request_t;

typedef struct {
    transaction_type_t type;

    client_id_t sender_id;
    client_id_t receiver_id;

    money_t amount;
} transaction_request_t;

typedef struct {
    client_id_t client_id;
} balance_request_t;

typedef struct {
    client_id_t client_id;
    money_t balance;
    uint64_t account_version;
} balance_response_t;

typedef struct {
    status_t status;
    uint32_t payload_size;
} response_t;

typedef struct {
    transaction_id_t transaction_id;

    transaction_state_t state;
    status_t status;

    money_t balance;
} transaction_response_t;

/* ---- Windows build additions (propose the same to the Linux team) ---- */
#define ACCOUNT_COUNT    3U
#define INITIAL_BALANCE  ((money_t)100000)   /* XAF per account */

#define PIPE_NAME  "\\\\.\\pipe\\BankPipe"
#define SHM_NAME   "Local\\BankSharedMemory"
#define SEM_NAME   "Local\\BankSemaphore"

/* This is what lives in shared memory. */
typedef struct {
    account_t accounts[ACCOUNT_COUNT];
} bank_t;

#endif /* PROTOCOL_H */
