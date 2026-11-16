#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "../server/server.h"
#include "../shared_memory/shared_memory.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WORKER_COUNT 4U
#define DEFAULT_ITERATIONS   500U
#define MAX_TEST_WORKERS     16U
#define MAX_TEST_ITERATIONS  100000U

typedef struct {
    char original_directory[PATH_MAX];
    char temporary_directory[PATH_MAX];
    int temporary_directory_created;
    int working_directory_changed;
    int shm_id;
    int queue_id;
    bank_t *bank;
    transaction_lock_t *lock;
} test_environment_t;

typedef struct {
    unsigned long vm_peak_kb;
    unsigned long vm_size_kb;
    unsigned long vm_rss_kb;
} proc_memory_t;

static int fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

static int setup_environment(test_environment_t *environment) {
    memset(environment, 0, sizeof(*environment));
    environment->shm_id = -1;
    environment->queue_id = -1;

    /* Keep test IPC keys and transaction logs away from a developer's real bank files. */
    if (getcwd(environment->original_directory, sizeof(environment->original_directory)) == NULL) {
        perror("getcwd");
        return 0;
    }

    snprintf(environment->temporary_directory, sizeof(environment->temporary_directory),
             "/tmp/crossipc-test-XXXXXX");
    if (mkdtemp(environment->temporary_directory) == NULL) {
        perror("mkdtemp");
        return 0;
    }
    environment->temporary_directory_created = 1;
    if (chdir(environment->temporary_directory) != 0) {
        perror("chdir temporary directory");
        return 0;
    }
    environment->working_directory_changed = 1;
    if (mkdir("utilities", 0700) != 0) {
        perror("mkdir utilities");
        return 0;
    }

    FILE *key_file = fopen(IPC_KEY_FILE, "wb");
    if (key_file == NULL) {
        perror("create IPC key file");
        return 0;
    }
    if (fclose(key_file) != 0) {
        perror("close IPC key file");
        return 0;
    }

    void *shared_address = NULL;
    status_t status = shm_create(IPC_KEY_FILE, SHM_PROJ_ID, sizeof(bank_t),
                                 &environment->shm_id, &shared_address);
    if (status != STATUS_OK) {
        fprintf(stderr, "shm_create failed: %s\n", server_status_string(status));
        return 0;
    }
    environment->bank = (bank_t *)shared_address;
    registry_init(environment->bank);

    status = semaphore_create(IPC_KEY_FILE, SEM_PROJ_ID, &environment->lock);
    if (status != STATUS_OK) {
        fprintf(stderr, "semaphore_create failed: %s\n", server_status_string(status));
        return 0;
    }

    status = msg_queue_create(IPC_KEY_FILE, MSG_PROJ_ID, &environment->queue_id);
    if (status != STATUS_OK) {
        fprintf(stderr, "msg_queue_create failed: %s\n", server_status_string(status));
        return 0;
    }

    return 1;
}

static int cleanup_environment(test_environment_t *environment) {
    int success = 1;

    if (environment->lock != NULL) {
        if (semaphore_remove(environment->lock) != STATUS_OK) {
            fprintf(stderr, "semaphore_remove failed during cleanup\n");
            success = 0;
        }
        environment->lock = NULL;
    }
    if (environment->queue_id >= 0) {
        if (msg_queue_remove(environment->queue_id) != STATUS_OK) {
            fprintf(stderr, "msg_queue_remove failed during cleanup\n");
            success = 0;
        }
        environment->queue_id = -1;
    }
    if (environment->bank != NULL) {
        if (shm_detach(environment->bank) != STATUS_OK) {
            fprintf(stderr, "shm_detach failed during cleanup\n");
            success = 0;
        }
        environment->bank = NULL;
    }
    if (environment->shm_id >= 0) {
        if (shm_remove(environment->shm_id) != STATUS_OK) {
            fprintf(stderr, "shm_remove failed during cleanup\n");
            success = 0;
        }
        environment->shm_id = -1;
    }

    if (environment->working_directory_changed) {
        if (unlink("utilities/transactions.log") != 0 && errno != ENOENT) {
            perror("remove test transaction log");
            success = 0;
        }
        if (unlink(IPC_KEY_FILE) != 0 && errno != ENOENT) {
            perror("remove test IPC key file");
            success = 0;
        }
        if (rmdir("utilities") != 0 && errno != ENOENT) {
            perror("remove test utilities directory");
            success = 0;
        }
        if (chdir(environment->original_directory) != 0) {
            perror("restore working directory");
            return 0;
        }
    }
    if (environment->temporary_directory_created &&
        rmdir(environment->temporary_directory) != 0) {
        perror("remove test temporary directory");
        success = 0;
    }

    return success;
}

static void make_request(Message *request, uint32_t request_id, int32_t operation,
                         const char *sender, const char *receiver, money_t amount) {
    memset(request, 0, sizeof(*request));
    request->request_id = request_id;
    request->sender_pid = (int32_t)getpid();
    request->operation = operation;
    request->amount = amount;
    snprintf(request->client_id, sizeof(request->client_id), "%s", sender);
    if (receiver != NULL) {
        snprintf(request->receiver_id, sizeof(request->receiver_id), "%s", receiver);
    }
}

static status_t execute_request(server_ctx_t *context, uint32_t request_id, int32_t operation,
                                const char *sender, const char *receiver, money_t amount,
                                Message *response) {
    Message request;
    make_request(&request, request_id, operation, sender, receiver, amount);
    memset(response, 0, sizeof(*response));
    return transaction_execute(context, &request, response);
}

static int check_status(const char *scenario, status_t actual, status_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s returned %s; expected %s\n",
                scenario, server_status_string(actual), server_status_string(expected));
        return 0;
    }
    fprintf(stderr, "PASS: %s\n", scenario);
    return 1;
}

static void terminate_and_reap(pid_t child) {
    if (kill(child, SIGTERM) != 0 && errno != ESRCH) {
        perror("terminate test child");
    }

    int child_status = 0;
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
}

static int run_message_queue_roundtrip(int queue_id) {
    pid_t child = fork();
    if (child < 0) {
        perror("fork message-queue client");
        return 0;
    }
    if (child == 0) {
        Message response;
        memset(&response, 0, sizeof(response));
        response.mtype = (long)getppid();

        int opened_queue_id = -1;
        status_t status = msg_queue_open(IPC_KEY_FILE, MSG_PROJ_ID, &opened_queue_id);
        int response_queue_id = queue_id;
        if (status == STATUS_OK) {
            response_queue_id = opened_queue_id;
            status = msg_queue_receive(opened_queue_id, &response, MSG_REQUEST_TYPE);
        }
        response.mtype = (long)getppid();
        response.status_code = status;
        _exit(msg_queue_send(response_queue_id, &response) == STATUS_OK ? 0 : 1);
    }

    Message request;
    memset(&request, 0, sizeof(request));
    request.mtype = MSG_REQUEST_TYPE;
    request.request_id = 42;
    request.sender_pid = (int32_t)getpid();
    request.operation = MSG_BALANCE;
    request.amount = 12345;
    snprintf(request.client_id, sizeof(request.client_id), "%s", "ACC0001");

    if (msg_queue_send(queue_id, &request) != STATUS_OK) {
        fprintf(stderr, "FAIL: enqueue message-queue request\n");
        terminate_and_reap(child);
        return 0;
    }

    Message response;
    memset(&response, 0, sizeof(response));
    if (msg_queue_receive(queue_id, &response, (long)getpid()) != STATUS_OK) {
        fprintf(stderr, "FAIL: receive routed message-queue response\n");
        terminate_and_reap(child);
        return 0;
    }

    int child_status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &child_status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child ||
        !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        return fail("message-queue client exits successfully");
    }
    if (response.status_code != STATUS_OK || response.request_id != request.request_id ||
        response.operation != request.operation || response.amount != request.amount ||
        strcmp(response.client_id, request.client_id) != 0) {
        return fail("message queue preserves request fields and PID reply routing");
    }
    fprintf(stderr, "PASS: System V message-queue request/reply round trip\n");
    return 1;
}

static int run_functional_scenarios(server_ctx_t *context) {
    Message response;
    balance_response_t balance;
    client_id_t account_id = context->bank->accounts[0].client_id;
    client_id_t second_account_id = context->bank->accounts[1].client_id;

    if (!registry_valid(context->bank) || !registry_consistent(context->bank, NULL, NULL)) {
        return fail("fresh bank state is valid and balanced");
    }

    if (!check_status("zero-amount deposit is rejected",
                      execute_request(context, 1, MSG_DEPOSIT, "ACC0001", NULL, 0, &response),
                      STATUS_INVALID_AMOUNT)) {
        return 0;
    }
    if (!check_status("negative-amount deposit is rejected",
                      execute_request(context, 10, MSG_DEPOSIT, "ACC0001", NULL, -1, &response),
                      STATUS_INVALID_AMOUNT)) {
        return 0;
    }
    if (!check_status("valid deposit succeeds",
                      execute_request(context, 2, MSG_DEPOSIT, "ACC0001", NULL, 1000, &response),
                      STATUS_OK)) {
        return 0;
    }
    char first_transaction_id[TXN_ID_LEN];
    snprintf(first_transaction_id, sizeof(first_transaction_id), "%s", response.txn_id);

    if (!check_status("duplicate request is idempotent",
                      execute_request(context, 2, MSG_DEPOSIT, "ACC0001", NULL, 1000, &response),
                      STATUS_OK) ||
        strcmp(first_transaction_id, response.txn_id) != 0) {
        return fail("retry returns the original transaction");
    }
    if (transaction_balance(context, &account_id, &balance) != STATUS_OK ||
        balance.balance != INITIAL_BALANCE + 1000) {
        return fail("duplicate request does not deposit twice");
    }

    if (!check_status("overdraft is rejected",
                      execute_request(context, 3, MSG_WITHDRAW, "ACC0001", NULL,
                                      INITIAL_BALANCE * 2, &response),
                      STATUS_INSUFFICIENT_FUNDS)) {
        return 0;
    }
    if (!check_status("self-transfer is rejected",
                      execute_request(context, 4, MSG_TRANSFER, "ACC0001", "ACC0001", 1, &response),
                      STATUS_SELF_TRANSFER)) {
        return 0;
    }
    if (!check_status("unknown transfer recipient is rejected",
                      execute_request(context, 5, MSG_TRANSFER, "ACC0001", "ACC9999", 1, &response),
                      STATUS_ACCOUNT_NOT_FOUND)) {
        return 0;
    }
    if (!check_status("transfer conserves total funds",
                      execute_request(context, 6, MSG_TRANSFER, "ACC0001", "ACC0002", 250, &response),
                      STATUS_OK)) {
        return 0;
    }
    if (transaction_balance(context, &account_id, &balance) != STATUS_OK ||
        balance.balance != INITIAL_BALANCE + 750) {
        return fail("transfer debits the sender by the requested amount");
    }
    if (transaction_balance(context, &second_account_id, &balance) != STATUS_OK ||
        balance.balance != INITIAL_BALANCE + 250) {
        return fail("transfer credits the recipient by the requested amount");
    }

    context->auto_approve = 0;
    if (!check_status("cash deposit waits for operator approval",
                      execute_request(context, 7, MSG_DEPOSIT, "ACC0001", NULL, 50, &response),
                      STATUS_TRANSACTION_PENDING)) {
        return 0;
    }

    pending_deposit_t pending[MAX_PENDING_DEPOSITS];
    int pending_count = transaction_get_pending(context, pending, MAX_PENDING_DEPOSITS);
    if (pending_count != 1 || pending[0].request_id != 7 ||
        transaction_approve_deposit(context, pending[0].txn_id) != STATUS_OK) {
        return fail("operator approves a queued deposit");
    }
    memset(&response, 0, sizeof(response));
    snprintf(response.client_id, sizeof(response.client_id), "%s", "ACC0001");
    if (transaction_status_query(context, 7, &response) != STATUS_OK ||
        response.status_code != STATUS_OK || response.amount != INITIAL_BALANCE + 800) {
        return fail("approved deposit status reports the committed balance");
    }
    fprintf(stderr, "PASS: approved deposit status is queryable\n");

    if (!check_status("second cash deposit enters the approval queue",
                      execute_request(context, 8, MSG_DEPOSIT, "ACC0001", NULL, 20, &response),
                      STATUS_TRANSACTION_PENDING)) {
        return 0;
    }
    pending_count = transaction_get_pending(context, pending, MAX_PENDING_DEPOSITS);
    if (pending_count != 1 || transaction_reject_deposit(context, pending[0].txn_id) != STATUS_OK) {
        return fail("operator rejects a queued deposit");
    }
    memset(&response, 0, sizeof(response));
    snprintf(response.client_id, sizeof(response.client_id), "%s", "ACC0001");
    if (transaction_status_query(context, 8, &response) != STATUS_OK ||
        response.status_code != STATUS_TRANSACTION_REJECTED) {
        return fail("rejected deposit status is queryable");
    }
    fprintf(stderr, "PASS: rejected deposit status is queryable\n");

    context->auto_approve = 1;
    if (!check_status("deposit that would overflow is rejected",
                      execute_request(context, 9, MSG_DEPOSIT, "ACC0001", NULL,
                                      INT64_MAX, &response),
                      STATUS_BALANCE_OVERFLOW)) {
        return 0;
    }
    if (!registry_consistent(context->bank, NULL, NULL)) {
        return fail("functional scenarios preserve the accounting invariant");
    }
    return 1;
}

static int parse_positive_argument(const char *text, unsigned long maximum, unsigned long *value) {
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 || parsed > maximum) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int read_proc_memory(proc_memory_t *memory) {
    FILE *status_file = fopen("/proc/self/status", "r");
    if (status_file == NULL) {
        perror("open /proc/self/status");
        return 0;
    }

    int found_peak = 0;
    int found_size = 0;
    int found_rss = 0;
    char line[256];
    while (fgets(line, sizeof(line), status_file) != NULL) {
        unsigned long value;
        if (sscanf(line, "VmPeak: %lu kB", &value) == 1) {
            memory->vm_peak_kb = value;
            found_peak = 1;
        } else if (sscanf(line, "VmSize: %lu kB", &value) == 1) {
            memory->vm_size_kb = value;
            found_size = 1;
        } else if (sscanf(line, "VmRSS: %lu kB", &value) == 1) {
            memory->vm_rss_kb = value;
            found_rss = 1;
        }
    }

    if (ferror(status_file)) {
        perror("read /proc/self/status");
        fclose(status_file);
        return 0;
    }
    if (fclose(status_file) != 0) {
        perror("close /proc/self/status");
        return 0;
    }
    return found_peak && found_size && found_rss;
}

static double timeval_milliseconds(struct timeval value) {
    return (double)value.tv_sec * 1000.0 + (double)value.tv_usec / 1000.0;
}

static int stress_worker(server_ctx_t context, unsigned worker_index, unsigned iterations) {
    context.worker_id = (int)worker_index + 1;
    context.auto_approve = 1;

    for (unsigned iteration = 0; iteration < iterations; iteration++) {
        uint32_t request_id = 10000U + worker_index * iterations + iteration;
        Message response;
        status_t status = execute_request(&context, request_id, MSG_DEPOSIT,
                                          "ACC0001", NULL, 1, &response);
        if (status != STATUS_OK) {
            fprintf(stderr, "worker %u request %u failed: %s\n",
                    worker_index, iteration, server_status_string(status));
            return 0;
        }
    }
    return 1;
}

static int run_stress_workload(server_ctx_t *context, unsigned workers, unsigned iterations,
                               struct rusage *children_before, struct rusage *children_after,
                               proc_memory_t *memory_before, proc_memory_t *memory_after,
                               double *elapsed_ms, long *worker_maxrss_kb) {
    if (getrusage(RUSAGE_CHILDREN, children_before) != 0) {
        perror("getrusage before workload");
        return 0;
    }
    if (!read_proc_memory(memory_before)) {
        return fail("read initial /proc/self/status memory metrics");
    }

    struct timespec start_time;
    struct timespec end_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        perror("clock_gettime start");
        return 0;
    }

    pid_t child_pids[MAX_TEST_WORKERS];
    unsigned children_started = 0;
    int success = 1;
    *worker_maxrss_kb = 0;
    /* Forked processes exercise the same shared-bank and kernel-lock path as Linux workers. */
    for (unsigned worker = 0; worker < workers; worker++) {
        pid_t child = fork();
        if (child < 0) {
            perror("fork stress worker");
            success = 0;
            break;
        }
        if (child == 0) {
            _exit(stress_worker(*context, worker, iterations) ? 0 : 1);
        }
        child_pids[children_started++] = child;
    }

    for (unsigned i = 0; i < children_started; i++) {
        int child_status = 0;
        struct rusage worker_usage;
        pid_t waited;
        do {
            waited = wait4(child_pids[i], &child_status, 0, &worker_usage);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            perror("wait4 stress worker");
            success = 0;
        } else {
            if (worker_usage.ru_maxrss > *worker_maxrss_kb) {
                *worker_maxrss_kb = worker_usage.ru_maxrss;
            }
            if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
                fprintf(stderr, "stress worker %u exited unsuccessfully\n", i);
                success = 0;
            }
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        perror("clock_gettime end");
        return 0;
    }
    if (getrusage(RUSAGE_CHILDREN, children_after) != 0) {
        perror("getrusage after workload");
        return 0;
    }
    if (!read_proc_memory(memory_after)) {
        return fail("read final /proc/self/status memory metrics");
    }

    *elapsed_ms = (double)(end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                  (double)(end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;

    if (children_started != workers) {
        success = 0;
    }
    return success;
}

static void print_measurement(unsigned workers, unsigned iterations, double elapsed_ms,
                              const struct rusage *before, const struct rusage *after,
                              const proc_memory_t *memory_before, const proc_memory_t *memory_after,
                              long worker_maxrss_kb) {
    /* Linux reports ru_maxrss in KiB; the /proc status fields are already in KiB. */
    printf("%u,%u,%u,%.3f,%.3f,%.3f,%ld,%ld,%ld,%ld,%ld,%lu,%lu,%lu,%lu,%lu\n",
           workers, iterations, workers * iterations, elapsed_ms,
           timeval_milliseconds(after->ru_utime) - timeval_milliseconds(before->ru_utime),
           timeval_milliseconds(after->ru_stime) - timeval_milliseconds(before->ru_stime),
           worker_maxrss_kb,
           after->ru_minflt - before->ru_minflt,
           after->ru_majflt - before->ru_majflt,
           after->ru_nvcsw - before->ru_nvcsw,
           after->ru_nivcsw - before->ru_nivcsw,
           memory_before->vm_size_kb, memory_before->vm_rss_kb,
           memory_after->vm_peak_kb, memory_after->vm_size_kb, memory_after->vm_rss_kb);
}

int main(int argc, char **argv) {
    unsigned long worker_count = DEFAULT_WORKER_COUNT;
    unsigned long iterations = DEFAULT_ITERATIONS;
    if ((argc > 1 && !parse_positive_argument(argv[1], MAX_TEST_WORKERS, &worker_count)) ||
        (argc > 2 && !parse_positive_argument(argv[2], MAX_TEST_ITERATIONS, &iterations)) ||
        argc > 3) {
        fprintf(stderr, "Usage: %s [workers: 1-%u] [iterations-per-worker: 1-%u]\n",
                argv[0], MAX_TEST_WORKERS, MAX_TEST_ITERATIONS);
        return 2;
    }

    test_environment_t environment;
    if (!setup_environment(&environment)) {
        cleanup_environment(&environment);
        return 1;
    }

    server_ctx_t context;
    memset(&context, 0, sizeof(context));
    context.bank = environment.bank;
    context.lock = environment.lock;
    context.use_lock = 1;
    context.auto_approve = 1;

    int success = run_message_queue_roundtrip(environment.queue_id);
    if (success) {
        success = run_functional_scenarios(&context);
    }

    struct rusage children_before;
    struct rusage children_after;
    proc_memory_t memory_before;
    proc_memory_t memory_after;
    double elapsed_ms = 0.0;
    long worker_maxrss_kb = 0;
    if (success) {
        success = run_stress_workload(&context, (unsigned)worker_count, (unsigned)iterations,
                                      &children_before, &children_after,
                                      &memory_before, &memory_after, &elapsed_ms,
                                      &worker_maxrss_kb);
    }

    if (success) {
        money_t expected_balance = INITIAL_BALANCE + 800 +
                                   (money_t)worker_count * (money_t)iterations;
        if (context.bank->accounts[0].balance != expected_balance ||
            context.bank->accounts[0].version != (uint64_t)(4U + worker_count * iterations) ||
            !registry_valid(context.bank) ||
            !registry_consistent(context.bank, NULL, NULL)) {
            success = fail("locked concurrent deposits preserve balances and the accounting invariant");
        } else {
            fprintf(stderr, "PASS: %u worker processes completed %u locked deposits each\n",
                    (unsigned)worker_count, (unsigned)iterations);
            fprintf(stderr, "PASS: final bank state satisfies the accounting invariant\n");
            print_measurement((unsigned)worker_count, (unsigned)iterations, elapsed_ms,
                              &children_before, &children_after, &memory_before, &memory_after,
                              worker_maxrss_kb);
        }
    }

    if (!cleanup_environment(&environment)) {
        success = 0;
    }
    return success ? 0 : 1;
}
