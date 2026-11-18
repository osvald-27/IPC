#include "../common/status.h"

/* Text for each status code, declared in common/status.h. */
const char *status_string(status_t status) {
    switch (status) {
    case STATUS_OK:                     return "ok";
    case STATUS_ERROR:                  return "error";
    case STATUS_INVALID_ARGUMENT:       return "invalid argument";
    case STATUS_OUT_OF_MEMORY:          return "out of memory";
    case STATUS_NOT_FOUND:              return "not found";
    case STATUS_ALREADY_EXISTS:         return "already exists";
    case STATUS_PERMISSION_DENIED:      return "permission denied";
    case STATUS_AUTH_FAILED:            return "login failed";
    case STATUS_SESSION_INVALID:        return "session not valid";
    case STATUS_SESSION_EXPIRED:        return "session expired";
    case STATUS_ACCOUNT_NOT_FOUND:      return "account not found";
    case STATUS_ACCOUNT_EXISTS:         return "account already exists";
    case STATUS_ACCOUNT_INACTIVE:       return "account inactive";
    case STATUS_INVALID_AMOUNT:         return "invalid amount";
    case STATUS_INSUFFICIENT_FUNDS:     return "insufficient funds";
    case STATUS_TRANSACTION_REJECTED:   return "transaction rejected";
    case STATUS_TRANSACTION_FAILED:     return "transaction failed";
    case STATUS_TRANSACTION_PENDING:    return "transaction pending";
    case STATUS_DUPLICATE_REQUEST:      return "duplicate request";
    case STATUS_SELF_TRANSFER:          return "cannot transfer to the same account";
    case STATUS_BALANCE_OVERFLOW:       return "balance too large";
    case STATUS_QUEUE_ERROR:            return "pipe error";
    case STATUS_QUEUE_FULL:             return "pipe full";
    case STATUS_QUEUE_EMPTY:            return "connection closed";
    case STATUS_SHARED_MEMORY_ERROR:    return "shared memory error";
    case STATUS_SYNCHRONIZATION_ERROR:  return "lock error";
    case STATUS_TIMEOUT:                return "timed out";
    case STATUS_STORAGE_ERROR:          return "storage error";
    case STATUS_RECOVERY_ERROR:         return "saved data could not be used";
    case STATUS_SERVER_UNAVAILABLE:     return "server not available";
    case STATUS_SHUTTING_DOWN:          return "shutting down";
    case STATUS_INTERRUPTED:            return "interrupted";
    case STATUS_INTERNAL_ERROR:         return "internal error";
    }
    return "unknown status";
}
