#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#include "types.h"
#include "status.h"
#include "transaction.h"

/* Number ("CRIP" in ASCII) lets the server quickly reject
 * corrupted or unrelated bytes from the message queue
 * before parsing the rest of the packet.
 */
#define PROTOCOL_NUMBER	0x43524950U
#define PROTOCOL_VERSION        1U

#define MAX_PAYLOAD_SIZE        4096U
#define MAX_MESSAGE_SIZE        8192U

/*
 * Operation codes should be spaced by tens (10s for queries, 20s for
 * money movements, etc.) so new commands can be added later
 * without shifting existing numbers.
 */
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

/*
 * Every request and response begins with this fixed header so the
 * receiver knows how many payload bytes to read next and which
 * session or request the message belongs to, abit complex but helps
 * helps with long term functionality.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint32_t message_type;
    uint32_t payload_size;

    request_id_t request_id;
    session_id_t session_id;
} message_header_t;

/*
 * Login payload carries only the client ID. Passwords or PINs stay
 * out of standard transaction packets and go through a dedicated
 * authentication exchange instead.
 */
typedef struct {
    client_id_t client_id;
} login_request_t;

/*
 * A transactional request arries the core terms needed to execute 
 * a deposit, withdrawal, or transfer. For deposits and withdrawals,
 * receiver_id can be left empty or matched to sender_id.
 */
typedef struct {
    transaction_type_t type;

    client_id_t sender_id;
    client_id_t receiver_id;

    money_t amount;
} transaction_request_t;

/*
 * Balance query payload.
 */
typedef struct {
    client_id_t client_id;
} balance_request_t;

/*
 * Returns the live balance alongside account_version so the client
 * can tell if another operation updated the account in the meantime.
 */
typedef struct {
    client_id_t client_id;
    money_t balance;
    uint64_t account_version;
} balance_response_t;

/*
 * Generic response header sent back when an operation only needs
 * to report success or an error code (such as logout or errors).
 */
typedef struct {
    status_t status;
    uint32_t payload_size;
} response_t;

/*
 * Sent back after a deposit, withdrawal, or transfer so the client
 * gets the generated transaction ID, current state, and new balance
 * in a single round trip.
 */
typedef struct {
    transaction_id_t transaction_id;

    transaction_state_t state;
    status_t status;

    money_t balance;
} transaction_response_t;

#endif /* PROTOCOL_H */
