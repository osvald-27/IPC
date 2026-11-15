
#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>

/* Protocol and system limits */
#define CLIENT_ID_LENGTH       7U
#define MAX_CLIENTS            1000U
#define MAX_TRANSACTION_ID     36U
#define MAX_REQUEST_ID         36U
#define MAX_SESSION_ID         36U

#define CURRENCY_CODE_LENGTH   3U
#define CURRENCY_CODE          "XAF"

/* In my first design I though of making
 *the type for money to be float but AI disagreed
 */
/* I use a signed interger here to allow the possibility to  
 * account for possible debt mechanisms, 64 bits also allows
 * for more money in magnitued of the amount, both keeping
 * precision in storage of small number and storing large ones
 * money_t makes it more niche to the concept of finances as
 * compared to int64_t
 */
typedef int64_t money_t;

/* It is easier to assign one client_id directly to another 
 * using =(id1 = id1) this is better than char* as there way less
 * overhead trying to dereference, this also guarantees fixed size
 * It also avoids the ambiguity of working with a pointer
 * the 1U is an unsigned integer for the null character adding
 * adding just 1 would be mixing signed and unsigned intergers
 */
typedef struct {
    char value[CLIENT_ID_LENGTH + 1U];
} client_id_t;

typedef struct {
    char value[MAX_TRANSACTION_ID + 1U];
} transaction_id_t;

typedef struct {
    char value[MAX_REQUEST_ID + 1U];
} request_id_t;

typedef struct {
    char value[MAX_SESSION_ID + 1U];
} session_id_t;

/*
 * Account state is more controlled on the server in
 * in shared memory. Clients modify these through safe 
 * structures.
 */
typedef struct {
    client_id_t client_id;

    money_t balance;
    money_t total_deposits;
    money_t total_withdrawals;

    uint64_t version;
    uint32_t flags;
    uint32_t reserved;
} account_t;

#endif /* TYPES_H */

