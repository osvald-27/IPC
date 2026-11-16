#define WIN32_LEAN_AND_MEAN
#include "../server/server.h"
#include "../shared_memory/shared_memory.h"

#include <direct.h>
#include <inttypes.h>
#include <limits.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DEFAULT_WORKERS 4U
#define DEFAULT_ITERATIONS 1000U
#define MAX_TEST_WORKERS 16U
#define MAX_TEST_ITERATIONS 100000U

typedef struct {
    server_ctx_t *context;
    unsigned worker_index;
    unsigned iterations;
    volatile LONG failures;
} worker_argument_t;

typedef struct {
    char original_directory[MAX_PATH];
    char temporary_root[MAX_PATH];
    char temporary_directory[MAX_PATH];
    int directory_created;
    int changed_directory;
    ShmRegion shared_bank;
    transaction_lock_t *lock;
    server_ctx_t context;
} test_environment_t;

typedef struct {
    FILETIME kernel_time;
    FILETIME user_time;
    PROCESS_MEMORY_COUNTERS_EX process_memory;
    MEMORYSTATUSEX system_memory;
} resource_sample_t;

static int fail(const char *scenario) {
    fprintf(stderr, "FAIL: %s\n", scenario);
    return 0;
}

static int expect_status(const char *scenario, status_t actual, status_t expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s returned %s; expected %s\n",
                scenario, server_status_string(actual), server_status_string(expected));
        return 0;
    }
    fprintf(stderr, "PASS: %s\n", scenario);
    return 1;
}

static void make_request(Message *request, uint32_t request_id, int32_t operation,
                         const char *sender, const char *receiver, money_t amount) {
    memset(request, 0, sizeof(*request));
    request->request_id = request_id;
    request->sender_pid = (int32_t)GetCurrentProcessId();
    request->operation = operation;
    request->amount = amount;
    snprintf(request->client_id, sizeof(request->client_id), "%s", sender);
    if (receiver != NULL) {
        snprintf(request->receiver_id, sizeof(request->receiver_id), "%s", receiver);
    }
}

static status_t execute_request(server_ctx_t *context, uint32_t request_id,
                                int32_t operation, const char *sender,
                                const char *receiver, money_t amount,
                                Message *response) {
    Message request;
    make_request(&request, request_id, operation, sender, receiver, amount);
    memset(response, 0, sizeof(*response));
    return transaction_execute(context, &request, response);
}

static int sample_resources(resource_sample_t *sample) {
    HANDLE process = GetCurrentProcess();
    FILETIME creation_time;
    FILETIME exit_time;

    memset(sample, 0, sizeof(*sample));
    sample->process_memory.cb = (DWORD)sizeof(sample->process_memory);
    sample->system_memory.dwLength = (DWORD)sizeof(sample->system_memory);
    if (!GetProcessTimes(process, &creation_time, &exit_time,
                         &sample->kernel_time, &sample->user_time)) {
        fprintf(stderr, "GetProcessTimes failed (error %lu)\n", GetLastError());
        return 0;
    }
    if (!GetProcessMemoryInfo(process,
                              (PROCESS_MEMORY_COUNTERS *)&sample->process_memory,
                              (DWORD)sizeof(sample->process_memory))) {
        fprintf(stderr, "GetProcessMemoryInfo failed (error %lu)\n", GetLastError());
        return 0;
    }
    if (!GlobalMemoryStatusEx(&sample->system_memory)) {
        fprintf(stderr, "GlobalMemoryStatusEx failed (error %lu)\n", GetLastError());
        return 0;
    }
    return 1;
}

static ULONGLONG filetime_ticks(FILETIME value) {
    ULARGE_INTEGER ticks;
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

static int setup_environment(test_environment_t *environment) {
    memset(environment, 0, sizeof(*environment));
    environment->shared_bank.handle = NULL;
    environment->shared_bank.view = NULL;

    DWORD cwd_length = GetCurrentDirectoryA((DWORD)sizeof(environment->original_directory),
                                           environment->original_directory);
    DWORD temp_length = GetTempPathA((DWORD)sizeof(environment->temporary_root),
                                    environment->temporary_root);
    if (cwd_length == 0 || cwd_length >= (DWORD)sizeof(environment->original_directory) ||
        temp_length == 0 || temp_length >= (DWORD)sizeof(environment->temporary_root)) {
        return fail("resolve the test and temporary directories");
    }

    if (GetTempFileNameA(environment->temporary_root, "cpi", 0,
                         environment->temporary_directory) == 0) {
        fprintf(stderr, "GetTempFileNameA failed (error %lu)\n", GetLastError());
        return 0;
    }
    if (!DeleteFileA(environment->temporary_directory)) {
        fprintf(stderr, "DeleteFileA temporary placeholder failed (error %lu)\n", GetLastError());
        return 0;
    }
    if (!CreateDirectoryA(environment->temporary_directory, NULL)) {
        fprintf(stderr, "CreateDirectoryA temporary test directory failed (error %lu)\n",
                GetLastError());
        return 0;
    }
    environment->directory_created = 1;
    if (!SetCurrentDirectoryA(environment->temporary_directory)) {
        fprintf(stderr, "SetCurrentDirectoryA failed (error %lu)\n", GetLastError());
        return 0;
    }
    environment->changed_directory = 1;
    if (!CreateDirectoryA("utilities", NULL)) {
        fprintf(stderr, "CreateDirectoryA utilities failed (error %lu)\n", GetLastError());
        return 0;
    }

    char object_name[96];
    snprintf(object_name, sizeof(object_name), "Local\\CrossIPC_Test_Bank_%lu",
             (unsigned long)GetCurrentProcessId());
    ShmStatus shared_status = shm_create(object_name, sizeof(bank_t),
                                         &environment->shared_bank);
    if (shared_status != SHM_OK) {
        fprintf(stderr, "shm_create failed with status %d\n", (int)shared_status);
        return 0;
    }

    char lock_name[96];
    snprintf(lock_name, sizeof(lock_name), "Local\\CrossIPC_Test_Lock_%lu",
             (unsigned long)GetCurrentProcessId());
    status_t lock_status = semaphore_create(lock_name, &environment->lock);
    if (lock_status != STATUS_OK) {
        fprintf(stderr, "semaphore_create failed: %s\n", server_status_string(lock_status));
        return 0;
    }

    environment->context.bank = (bank_t *)environment->shared_bank.view;
    environment->context.lock = environment->lock;
    environment->context.worker_id = 0;
    environment->context.pid = GetCurrentProcessId();
    environment->context.use_lock = 1;
    environment->context.auto_approve = 1;
    registry_init(environment->context.bank);
    return 1;
}

static int delete_file_if_present(const char *path) {
    if (DeleteFileA(path)) {
        return 1;
    }
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return 1;
    }
    fprintf(stderr, "DeleteFileA %s failed (error %lu)\n", path, error);
    return 0;
}

static int remove_directory_if_present(const char *path) {
    if (RemoveDirectoryA(path)) {
        return 1;
    }
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return 1;
    }
    fprintf(stderr, "RemoveDirectoryA %s failed (error %lu)\n", path, error);
    return 0;
}

static int cleanup_environment(test_environment_t *environment) {
    int success = 1;

    if (environment->lock != NULL) {
        semaphore_remove(environment->lock);
        environment->lock = NULL;
    }
    if (environment->shared_bank.handle != NULL ||
        environment->shared_bank.view != NULL) {
        shm_close(&environment->shared_bank);
    }

    if (environment->changed_directory &&
        !SetCurrentDirectoryA(environment->original_directory)) {
        fprintf(stderr, "restore working directory failed (error %lu)\n", GetLastError());
        return 0;
    }
    if (environment->directory_created) {
        char path[MAX_PATH];
        int length = snprintf(path, sizeof(path), "%s\\utilities\\bank.dat",
                              environment->temporary_directory);
        if (length < 0 || length >= (int)sizeof(path) || !delete_file_if_present(path)) {
            success = 0;
        }
        length = snprintf(path, sizeof(path), "%s\\utilities\\bank.dat.tmp",
                          environment->temporary_directory);
        if (length < 0 || length >= (int)sizeof(path) || !delete_file_if_present(path)) {
            success = 0;
        }
        length = snprintf(path, sizeof(path), "%s\\utilities\\transactions.log",
                          environment->temporary_directory);
        if (length < 0 || length >= (int)sizeof(path) || !delete_file_if_present(path)) {
            success = 0;
        }
        length = snprintf(path, sizeof(path), "%s\\utilities",
                          environment->temporary_directory);
        if (length < 0 || length >= (int)sizeof(path) ||
            !remove_directory_if_present(path)) {
            success = 0;
        }
        if (!remove_directory_if_present(environment->temporary_directory)) {
            success = 0;
        }
    }
    return success;
}

static int test_authentication(server_ctx_t *context) {
    session_t session;
    memset(&session, 0, sizeof(session));

    if (!expect_status("unknown account login is denied",
                       auth_login(context, &session, "ACC9999", GetCurrentProcessId()),
                       STATUS_AUTH_FAILED) ||
        !expect_status("registered account login succeeds",
                       auth_login(context, &session, "ACC0001", GetCurrentProcessId()),
                       STATUS_OK)) {
        return 0;
    }
    if (!session.logged_in || session.session_id.value[0] == '\0') {
        return fail("successful login creates an active session token");
    }
    char session_token[MAX_SESSION_ID + 1U];
    snprintf(session_token, sizeof(session_token), "%s", session.session_id.value);
    if (!expect_status("matching account and token authenticate",
                       auth_check(context, "ACC0001", session_token),
                       STATUS_OK) ||
        !expect_status("another account cannot reuse the session",
                       auth_check(context, "ACC0002", session_token),
                       STATUS_SESSION_INVALID) ||
        !expect_status("another token cannot reuse the session",
                       auth_check(context, "ACC0001", "invalid-session"),
                       STATUS_SESSION_INVALID)) {
        return 0;
    }
    auth_logout(context, session_token);
    return expect_status("logged-out session is rejected",
                         auth_check(context, "ACC0001", session_token),
                         STATUS_SESSION_INVALID);
}

static int test_transactions(server_ctx_t *context) {
    Message response;
    balance_response_t balance;
    client_id_t sender = context->bank->accounts[0].client_id;
    client_id_t recipient = context->bank->accounts[1].client_id;
    client_id_t unknown;
    memset(&unknown, 0, sizeof(unknown));
    snprintf(unknown.value, sizeof(unknown.value), "%s", "ACC9999");

    if (!expect_status("initial balance query succeeds",
                       transaction_balance(context, &sender, &balance), STATUS_OK) ||
        balance.balance != INITIAL_BALANCE ||
        !expect_status("unknown account balance query is rejected",
                       transaction_balance(context, &unknown, &balance),
                       STATUS_ACCOUNT_NOT_FOUND)) {
        return fail("initial and unknown-account balance results are correct");
    }

    if (!expect_status("zero-value deposit is rejected",
                       execute_request(context, 1, MSG_DEPOSIT, "ACC0001", NULL, 0, &response),
                       STATUS_INVALID_AMOUNT) ||
        !expect_status("negative-value deposit is rejected",
                       execute_request(context, 2, MSG_DEPOSIT, "ACC0001", NULL, -1, &response),
                       STATUS_INVALID_AMOUNT)) {
        return 0;
    }

    if (!expect_status("valid deposit succeeds",
                       execute_request(context, 10, MSG_DEPOSIT, "ACC0001", NULL, 1000, &response),
                       STATUS_OK)) {
        return 0;
    }
    char original_transaction_id[TXN_ID_LEN];
    snprintf(original_transaction_id, sizeof(original_transaction_id), "%s", response.txn_id);
    if (!expect_status("retry with the same request ID is idempotent",
                       execute_request(context, 10, MSG_DEPOSIT, "ACC0001", NULL, 1000, &response),
                       STATUS_OK) ||
        strcmp(original_transaction_id, response.txn_id) != 0 ||
        transaction_balance(context, &sender, &balance) != STATUS_OK ||
        balance.balance != INITIAL_BALANCE + 1000) {
        return fail("idempotent retry preserves the original transaction and balance");
    }

    if (!expect_status("valid withdrawal succeeds",
                       execute_request(context, 11, MSG_WITHDRAW, "ACC0001", NULL, 250, &response),
                       STATUS_OK) ||
        !expect_status("overdraft is rejected",
                       execute_request(context, 12, MSG_WITHDRAW, "ACC0001", NULL,
                                       INITIAL_BALANCE * 2, &response),
                       STATUS_INSUFFICIENT_FUNDS) ||
        !expect_status("self-transfer is rejected",
                       execute_request(context, 13, MSG_TRANSFER, "ACC0001", "ACC0001", 1, &response),
                       STATUS_SELF_TRANSFER) ||
        !expect_status("unknown transfer recipient is rejected",
                       execute_request(context, 14, MSG_TRANSFER, "ACC0001", "ACC9999", 1, &response),
                       STATUS_ACCOUNT_NOT_FOUND) ||
        !expect_status("valid transfer succeeds",
                       execute_request(context, 15, MSG_TRANSFER, "ACC0001", "ACC0002", 200, &response),
                       STATUS_OK)) {
        return 0;
    }
    if (transaction_balance(context, &sender, &balance) != STATUS_OK ||
        balance.balance != INITIAL_BALANCE + 550 ||
        transaction_balance(context, &recipient, &balance) != STATUS_OK ||
        balance.balance != INITIAL_BALANCE + 200 ||
        !registry_consistent(context->bank, NULL, NULL)) {
        return fail("withdrawal and transfer balances satisfy the bank invariant");
    }
    fprintf(stderr, "PASS: transaction mutations preserve balances and bank totals\n");

    context->auto_approve = 0;
    if (!expect_status("deposit waits for operator approval",
                       execute_request(context, 16, MSG_DEPOSIT, "ACC0001", NULL, 50, &response),
                       STATUS_TRANSACTION_PENDING)) {
        return 0;
    }
    pending_deposit_t pending[MAX_PENDING_DEPOSITS];
    int pending_count = transaction_get_pending(context, pending, MAX_PENDING_DEPOSITS);
    if (pending_count != 1 || pending[0].request_id != 16 ||
        !expect_status("operator approves pending deposit",
                       transaction_approve_deposit(context, pending[0].txn_id), STATUS_OK)) {
        return fail("pending deposit approval succeeds");
    }
    memset(&response, 0, sizeof(response));
    snprintf(response.client_id, sizeof(response.client_id), "%s", "ACC0001");
    if (transaction_status_query(context, 16, &response) != STATUS_OK ||
        response.status_code != STATUS_OK || response.amount != INITIAL_BALANCE + 600) {
        return fail("approved-deposit status includes its committed balance");
    }
    fprintf(stderr, "PASS: approved deposit result can be queried\n");

    if (!expect_status("second deposit waits for operator approval",
                       execute_request(context, 17, MSG_DEPOSIT, "ACC0001", NULL, 30, &response),
                       STATUS_TRANSACTION_PENDING)) {
        return 0;
    }
    pending_count = transaction_get_pending(context, pending, MAX_PENDING_DEPOSITS);
    if (pending_count != 1 ||
        !expect_status("operator rejects pending deposit",
                       transaction_reject_deposit(context, pending[0].txn_id), STATUS_OK)) {
        return fail("pending deposit rejection succeeds");
    }
    memset(&response, 0, sizeof(response));
    snprintf(response.client_id, sizeof(response.client_id), "%s", "ACC0001");
    if (transaction_status_query(context, 17, &response) != STATUS_OK ||
        response.status_code != STATUS_TRANSACTION_REJECTED) {
        return fail("rejected-deposit status can be queried");
    }
    fprintf(stderr, "PASS: rejected deposit result can be queried\n");

    context->auto_approve = 1;
    registry_init(context->bank);
    context->bank->accounts[0].balance = INT64_MAX;
    context->bank->accounts[0].total_deposits = INT64_MAX - INITIAL_BALANCE;
    context->bank->accounts[1].balance = 0;
    context->bank->accounts[1].total_withdrawals = INITIAL_BALANCE;
    context->bank->accounts[2].balance = 0;
    context->bank->accounts[2].total_withdrawals = INITIAL_BALANCE;
    if (!registry_consistent(context->bank, NULL, NULL) ||
        !expect_status("deposit that exceeds INT64_MAX is rejected",
                       execute_request(context, 18, MSG_DEPOSIT, "ACC0001", NULL, 1, &response),
                       STATUS_BALANCE_OVERFLOW) ||
        context->bank->accounts[0].balance != INT64_MAX ||
        !registry_consistent(context->bank, NULL, NULL)) {
        return fail("overflow rejection does not mutate or corrupt account totals");
    }
    fprintf(stderr, "PASS: overflowing deposit leaves the bank unchanged\n");
    registry_init(context->bank);
    return 1;
}

static int test_persistence(bank_t *bank) {
    bank->accounts[0].balance += 1234;
    bank->accounts[0].total_deposits += 1234;
    bank->accounts[0].version++;

    if (!expect_status("bank snapshot saves successfully",
                       persistence_save(bank), STATUS_OK)) {
        return 0;
    }

    bank_t loaded;
    registry_init(&loaded);
    if (!expect_status("saved bank snapshot loads successfully",
                       persistence_load(&loaded), STATUS_OK) ||
        loaded.accounts[0].balance != bank->accounts[0].balance ||
        !registry_consistent(&loaded, NULL, NULL)) {
        return fail("loaded bank snapshot matches the saved balances");
    }

    bank->accounts[0].balance += 4321;
    bank->accounts[0].total_deposits += 4321;
    bank->accounts[0].version++;
    if (!expect_status("existing bank snapshot is replaced",
                       persistence_save(bank), STATUS_OK)) {
        return 0;
    }
    registry_init(&loaded);
    if (persistence_load(&loaded) != STATUS_OK ||
        loaded.accounts[0].balance != bank->accounts[0].balance) {
        return fail("replacement bank snapshot can be loaded");
    }

    FILE *damaged_file = fopen(DATA_FILE, "wb");
    if (damaged_file == NULL) {
        return fail("open bank snapshot for the truncation test");
    }
    int write_failed = fputc('x', damaged_file) == EOF;
    int close_failed = fclose(damaged_file) != 0;
    if (write_failed || close_failed) {
        return fail("write deliberately truncated bank snapshot");
    }
    return expect_status("truncated bank snapshot is rejected",
                         persistence_load(&loaded), STATUS_RECOVERY_ERROR);
}

static DWORD WINAPI deposit_worker(LPVOID parameter) {
    worker_argument_t *worker = (worker_argument_t *)parameter;
    server_ctx_t context = *worker->context;
    context.worker_id = (int)worker->worker_index + 1;

    for (unsigned iteration = 0; iteration < worker->iterations; iteration++) {
        uint32_t request_id = 10000U +
                              worker->worker_index * worker->iterations +
                              iteration;
        Message response;
        status_t status = execute_request(&context, request_id, MSG_DEPOSIT,
                                          "ACC0001", NULL, 1, &response);
        if (status != STATUS_OK) {
            fprintf(stderr, "worker %u request %u failed: %s\n",
                    worker->worker_index, iteration, server_status_string(status));
            InterlockedIncrement(&worker->failures);
            return 1;
        }
    }
    return 0;
}

static int parse_positive(const char *text, unsigned maximum, unsigned *value) {
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (text[0] == '\0' || end == text || *end != '\0' ||
        parsed == 0 || parsed > maximum) {
        return 0;
    }
    *value = (unsigned)parsed;
    return 1;
}

static int run_stress(test_environment_t *environment, unsigned workers,
                      unsigned iterations, unsigned repeat) {
    HANDLE threads[MAX_TEST_WORKERS];
    worker_argument_t arguments[MAX_TEST_WORKERS];
    unsigned started = 0;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER finish;
    resource_sample_t before;
    resource_sample_t after;

    if (!sample_resources(&before) ||
        !QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&start)) {
        return fail("collect starting performance counters");
    }

    for (unsigned i = 0; i < workers; i++) {
        memset(&arguments[i], 0, sizeof(arguments[i]));
        arguments[i].context = &environment->context;
        arguments[i].worker_index = i;
        arguments[i].iterations = iterations;
        threads[i] = CreateThread(NULL, 0, deposit_worker, &arguments[i], 0, NULL);
        if (threads[i] == NULL) {
            fprintf(stderr, "CreateThread worker %u failed (error %lu)\n", i, GetLastError());
            break;
        }
        started++;
    }

    int success = started == workers;
    if (started > 0) {
        DWORD wait_result = WaitForMultipleObjects(started, threads, TRUE, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "WaitForMultipleObjects failed (result %lu, error %lu)\n",
                    wait_result, GetLastError());
            success = 0;
        }
    }
    for (unsigned i = 0; i < started; i++) {
        DWORD exit_code = 1;
        if (!GetExitCodeThread(threads[i], &exit_code) || exit_code != 0 ||
            arguments[i].failures != 0) {
            success = 0;
        }
        CloseHandle(threads[i]);
    }

    if (!QueryPerformanceCounter(&finish) || !sample_resources(&after)) {
        return fail("collect ending performance counters");
    }
    if (!success) {
        return 0;
    }

    bank_t *bank = environment->context.bank;
    uint64_t transactions = (uint64_t)workers * (uint64_t)iterations;
    money_t expected_balance = INITIAL_BALANCE + (money_t)transactions;
    if (bank->accounts[0].balance != expected_balance ||
        bank->accounts[0].total_deposits != (money_t)transactions ||
        bank->accounts[0].version != 1U + transactions ||
        !registry_valid(bank) ||
        !registry_consistent(bank, NULL, NULL)) {
        return fail("locked concurrent deposits preserve balances and accounting invariants");
    }

    double elapsed_ms = (double)(finish.QuadPart - start.QuadPart) * 1000.0 /
                        (double)frequency.QuadPart;
    double user_ms = (double)(filetime_ticks(after.user_time) -
                              filetime_ticks(before.user_time)) / 10000.0;
    double kernel_ms = (double)(filetime_ticks(after.kernel_time) -
                                filetime_ticks(before.kernel_time)) / 10000.0;
    printf("%u,%u,%u,%" PRIu64 ",%.3f,%.3f,%.3f,%zu,%zu,%zu,%zu,%zu,%zu\n",
           repeat, workers, iterations, transactions, elapsed_ms, user_ms, kernel_ms,
           (size_t)before.process_memory.WorkingSetSize / 1024U,
           (size_t)after.process_memory.WorkingSetSize / 1024U,
           (size_t)after.process_memory.PeakWorkingSetSize / 1024U,
           (size_t)before.process_memory.PageFaultCount,
           (size_t)after.process_memory.PageFaultCount,
           (size_t)after.system_memory.ullAvailPhys / 1024U);
    fprintf(stderr, "PASS: %u worker threads completed %u deposits each; final bank is consistent\n",
            workers, iterations);
    return 1;
}

int main(int argc, char **argv) {
    unsigned     workers = TEST_DEFAULT_WORKERS;
    unsigned iterations = DEFAULT_ITERATIONS;
    unsigned repeat = 1;
    if ((argc > 1 && !parse_positive(argv[1], MAX_TEST_WORKERS, &workers)) ||
        (argc > 2 && !parse_positive(argv[2], MAX_TEST_ITERATIONS, &iterations)) ||
        (argc > 3 && !parse_positive(argv[3], 100U, &repeat)) ||
        argc > 4) {
        fprintf(stderr, "Usage: %s [workers: 1-%u] [iterations: 1-%u] [repeat: 1-100]\n",
                argv[0], MAX_TEST_WORKERS, MAX_TEST_ITERATIONS);
        return 2;
    }

    test_environment_t environment;
    if (!setup_environment(&environment)) {
        cleanup_environment(&environment);
        return 1;
    }

    int success = test_authentication(&environment.context) &&
                  test_transactions(&environment.context) &&
                  test_persistence(environment.context.bank);

    registry_init(environment.context.bank);
    if (success) {
        success = run_stress(&environment, workers, iterations, repeat);
    }

    int cleanup_success = cleanup_environment(&environment);
    return success && cleanup_success ? 0 : 1;
}
