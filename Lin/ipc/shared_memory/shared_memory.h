#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "../common/types.h"
#include "../common/status.h"

/*
 * Anchor file and project characters used by ftok() to generate System V keys.
 * Using 'S' for the server's authoritative memory and 'P' for the published
 * read-only client snapshot keeps the two kernel segments completely isolated.
 */
#define DEFAULT_SHM_KEY_FILE "utilities/ipc.key"
#define SHM_PROJ_ID          'S'
#define SHM_SNAPSHOT_PROJ_ID 'P'

#define SNAPSHOT_MAX_ACCOUNTS 3U

/*
 * Sanitized per-account view exposed to local clients.
 * Contains only safe, public fields—never password hashes, salts, keys, or
 * internal server queues.
 */
typedef struct {
    client_id_t client_id;
    money_t     balance;
    money_t     total_deposits;
    money_t     total_withdrawals;
    uint64_t    version;
} account_snapshot_t;

/*
 * Read-only snapshot published by the server for local clients.
 * 'sequence' is incremented before and after each server update (seqlock pattern):
 *   - Odd value  -> server is currently writing an update (client retries)
 *   - Even value -> snapshot is stable; if sequence matches before and after
 *                   reading, the client knows it did not observe a torn read.
 */
typedef struct {
    uint64_t           sequence;
    uint32_t           account_count;
    uint32_t           reserved;
    account_snapshot_t accounts[SNAPSHOT_MAX_ACCOUNTS];
} bank_snapshot_t;

/*
 * Creates a brand-new System V shared-memory segment of 'size' bytes with
 * owner-only permissions (0600) and maps it into the calling process with
 * full read/write permissions. Called once by the server at startup.
 */
status_t shm_create(const char *key_path, char proj_id, size_t size, int *shm_id, void **addr);

/*
 * Attaches to an existing shared-memory segment created by the server.
 * Passing read_only = 1 maps the pages with SHM_RDONLY so the OS kernel
 * hardware-blocks any write attempts from a client process.
 */
status_t shm_open(const char *key_path, char proj_id, size_t size, int read_only, int *shm_id, void **addr);

/*
 * Unmaps the shared-memory segment from the calling process's virtual
 * address space without deleting the underlying kernel segment.
 */
status_t shm_detach(const void *addr);

/*
 * Marks the shared-memory segment for destruction in the kernel during
 * server shutdown. Linux frees the physical RAM once every attached process
 * detaches or exits.
 */
status_t shm_remove(int shm_id);

/*
 * Helper called by the server inside its critical section to copy live
 * account figures into the sanitized client snapshot safely.
 */
void shm_publish_snapshot(bank_snapshot_t *snapshot, const account_t *accounts, uint32_t count);

/*
 * Helper called by a local client to read a single account's published state
 * without tearing, retrying automatically if the server is mid-update.
 */
status_t shm_read_account_snapshot(const bank_snapshot_t *snapshot, const client_id_t *id, account_snapshot_t *out);

#endif /* SHARED_MEMORY_H */
