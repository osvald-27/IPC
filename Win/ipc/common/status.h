#ifndef STATUS_H
#define STATUS_H

/* Status codes are grouped by negative ranges (-10s for auth,
 * -20s for accounts, -30s for transactions, etc.) so any return
 * value >= 0 means success while any negative value flags a
 * specific failure category immediately.
 */
typedef enum {
    STATUS_OK = 0,

    /* General errors */
    STATUS_ERROR               = -1,
    STATUS_INVALID_ARGUMENT    = -2,
    STATUS_OUT_OF_MEMORY       = -3,
    STATUS_NOT_FOUND           = -4,
    STATUS_ALREADY_EXISTS      = -5,
    STATUS_PERMISSION_DENIED   = -6,

    /* Authentication and session */
    STATUS_AUTH_FAILED         = -10,
    STATUS_SESSION_INVALID     = -11,
    STATUS_SESSION_EXPIRED     = -12,

    /* Account errors */
    STATUS_ACCOUNT_NOT_FOUND   = -20,
    STATUS_ACCOUNT_EXISTS      = -21,
    STATUS_ACCOUNT_INACTIVE    = -22,

    /* Transaction errors */
    STATUS_INVALID_AMOUNT      = -30,
    STATUS_INSUFFICIENT_FUNDS  = -31,
    STATUS_TRANSACTION_REJECTED = -32,
    STATUS_TRANSACTION_FAILED  = -33,
    STATUS_TRANSACTION_PENDING = -34,
    STATUS_DUPLICATE_REQUEST   = -35,
    STATUS_SELF_TRANSFER       = -36,
    STATUS_BALANCE_OVERFLOW    = -37,

    /* IPC errors */
    STATUS_QUEUE_ERROR         = -40,
    STATUS_QUEUE_FULL          = -41,
    STATUS_QUEUE_EMPTY         = -42,
    STATUS_SHARED_MEMORY_ERROR = -43,
    STATUS_SYNCHRONIZATION_ERROR = -44,
    STATUS_TIMEOUT             = -45,

    /* Persistence */
    STATUS_STORAGE_ERROR       = -50,
    STATUS_RECOVERY_ERROR      = -51,

    /* Lifecycle */
    STATUS_SERVER_UNAVAILABLE  = -60,
    STATUS_SHUTTING_DOWN       = -61,
    STATUS_INTERRUPTED         = -62,

    STATUS_INTERNAL_ERROR      = -70

} status_t;

/* Converts a status enum value into a readable text string
 * so clients and server logs do not have to print raw numbers.
 */
const char *status_string(status_t status);

#endif /* STATUS_H */
