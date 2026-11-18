#define WIN32_LEAN_AND_MEAN
#include "../server/server.h"

#include <inttypes.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_WORKERS 8U
#define DEFAULT_ITERATIONS 5000U
#define MAX_TEST_WORKERS 16U
#define MAX_TEST_ITERATIONS 200000U

typedef struct {
    bank_t *bank;
    unsigned worker_index;
    unsigned iterations;
} worker_argument_t;

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

static int delete_file_if_present(const char *path) {
    if (DeleteFileA(path)) {
        return 1;
    }

    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return 1;
    }
    fprintf(stderr, "FAIL: delete %s (error %lu)\n", path, error);
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
    fprintf(stderr, "FAIL: remove directory %s (error %lu)\n", path, error);
    return 0;
}

static int test_registry(bank_t *bank) {
    registry_init(NULL);
    registry_init(bank);

    if (!registry_valid(bank) || !registry_consistent(bank, NULL, NULL)) {
        return fail("initialized accounts are valid and balanced");
    }

    money_t expected_total = (money_t)ACCOUNT_COUNT * INITIAL_BALANCE;
    money_t actual_total = 0;
    money_t ledger_total = 0;
    if (!registry_consistent(bank, &actual_total, &ledger_total) ||
        actual_total != expected_total || ledger_total != expected_total) {
        return fail("initial account total matches the accounting ledger");
    }

    for (unsigned i = 0; i < ACCOUNT_COUNT; i++) {
        client_id_t expected_id;
        registry_make_id(&expected_id, i);
        account_t *account = registry_find(bank, &expected_id);
        if (account == NULL || account->balance != INITIAL_BALANCE || account->version != 1) {
            return fail("generated account IDs and initial balances are correct");
        }
    }
    if (registry_find(bank, NULL) != NULL) {
        return fail("null account lookup is rejected");
    }

    bank->accounts[0].balance++;
    if (registry_consistent(bank, NULL, NULL)) {
        return fail("accounting audit detects an unexplained balance change");
    }
    bank->accounts[0].balance--;

    client_id_t saved_id = bank->accounts[0].client_id;
    snprintf(bank->accounts[0].client_id.value,
             sizeof(bank->accounts[0].client_id.value), "%s", "ACC9999");
    if (registry_valid(bank)) {
        return fail("registry validation detects a noncanonical account ID");
    }
    bank->accounts[0].client_id = saved_id;

    bank->accounts[0].balance = -1;
    if (registry_valid(bank)) {
        return fail("registry validation detects a negative balance");
    }
    bank->accounts[0].balance = INITIAL_BALANCE;

    if (!registry_valid(bank) || !registry_consistent(bank, NULL, NULL)) {
        return fail("restored registry passes validation");
    }
    fprintf(stderr, "PASS: Windows account registry and balance invariants\n");
    return 1;
}

static int test_authentication(bank_t *bank) {
    server_ctx_t context;
    session_t session;
    memset(&context, 0, sizeof(context));
    memset(&session, 0, sizeof(session));
    context.bank = bank;
    context.worker_id = 1;
    context.pid = GetCurrentProcessId();

    if (!expect_status("unknown account login is denied",
                       auth_login(&context, &session, "ACC9999", GetCurrentProcessId()),
                       STATUS_AUTH_FAILED)) {
        return 0;
    }
    if (!expect_status("registered account login succeeds",
                       auth_login(&context, &session, "ACC0001", GetCurrentProcessId()),
                       STATUS_OK)) {
        return 0;
    }
    if (!session.logged_in || session.session_id.value[0] == '\0') {
        return fail("successful login creates an active session token");
    }
    if (!expect_status("matching client and session authenticate",
                       auth_check(&session, "ACC0001", session.session_id.value),
                       STATUS_OK) ||
        !expect_status("different account cannot reuse the session",
                       auth_check(&session, "ACC0002", session.session_id.value),
                       STATUS_SESSION_INVALID) ||
        !expect_status("different token cannot reuse the session",
                       auth_check(&session, "ACC0001", "invalid-session"),
                       STATUS_SESSION_INVALID) ||
        !expect_status("session token alone is sufficient on its bound pipe",
                       auth_check(&session, NULL, session.session_id.value),
                       STATUS_OK)) {
        return 0;
    }

    auth_logout(&session);
    if (session.logged_in || session.session_id.value[0] != '\0') {
        return fail("logout clears the session and token");
    }
    if (!expect_status("logged-out session is rejected",
                       auth_check(&session, "ACC0001", "any-token"),
                       STATUS_SESSION_INVALID)) {
        return 0;
    }
    fprintf(stderr, "PASS: Windows authentication and session lifecycle\n");
    return 1;
}

static int test_persistence(void) {
    char original_directory[MAX_PATH];
    char temporary_root[MAX_PATH];
    char temporary_directory[MAX_PATH];
    DWORD original_length = GetCurrentDirectoryA((DWORD)sizeof(original_directory),
                                                  original_directory);
    DWORD root_length = GetTempPathA((DWORD)sizeof(temporary_root), temporary_root);
    if (original_length == 0 || original_length >= (DWORD)sizeof(original_directory) ||
        root_length == 0 || root_length >= (DWORD)sizeof(temporary_root)) {
        return fail("resolve working and temporary directories");
    }
    /* Persistence uses the process working directory, so isolate it from real bank data. */
    if (GetTempFileNameA(temporary_root, "ipc", 0, temporary_directory) == 0) {
        return fail("create isolated persistence test directory name");
    }
    if (!DeleteFileA(temporary_directory)) {
        fprintf(stderr, "FAIL: remove temporary file placeholder (error %lu)\n", GetLastError());
        return 0;
    }
    if (!CreateDirectoryA(temporary_directory, NULL)) {
        fprintf(stderr, "FAIL: create isolated persistence directory (error %lu)\n", GetLastError());
        return 0;
    }

    int success = 1;
    int changed_directory = 0;
    int utilities_created = 0;
    if (!SetCurrentDirectoryA(temporary_directory)) {
        success = fail("enter isolated persistence test directory");
        goto cleanup;
    }
    changed_directory = 1;
    if (!CreateDirectoryA("utilities", NULL)) {
        success = fail("create isolated utilities directory");
        goto cleanup;
    }
    utilities_created = 1;

    bank_t original;
    bank_t loaded;
    registry_init(&original);
    original.accounts[0].balance += 1234;
    original.accounts[0].total_deposits = 1234;
    original.accounts[0].version++;

    if (!expect_status("save a bank snapshot",
                       persistence_save(&original), STATUS_OK)) {
        success = 0;
        goto cleanup;
    }

    registry_init(&loaded);
    if (!expect_status("load a saved bank snapshot",
                       persistence_load(&loaded), STATUS_OK) ||
        loaded.accounts[0].balance != original.accounts[0].balance ||
        !registry_consistent(&loaded, NULL, NULL)) {
        success = fail("loaded snapshot preserves balances and ledger consistency");
        goto cleanup;
    }

    original.accounts[0].balance += 4321;
    original.accounts[0].total_deposits += 4321;
    original.accounts[0].version++;
    if (!expect_status("atomically replace an existing bank snapshot",
                       persistence_save(&original), STATUS_OK)) {
        success = 0;
        goto cleanup;
    }
    registry_init(&loaded);
    if (persistence_load(&loaded) != STATUS_OK ||
        loaded.accounts[0].balance != original.accounts[0].balance) {
        success = fail("replacement snapshot can be loaded");
        goto cleanup;
    }

    FILE *damaged_file = fopen(DATA_FILE, "wb");
    if (damaged_file == NULL) {
        success = fail("open snapshot for corruption test");
        goto cleanup;
    }
    int write_failed = (fputc('x', damaged_file) == EOF);
    int close_failed = (fclose(damaged_file) != 0);
    if (write_failed || close_failed) {
        success = fail("write a deliberately truncated snapshot");
        goto cleanup;
    }
    if (!expect_status("reject a truncated bank snapshot",
                       persistence_load(&loaded), STATUS_RECOVERY_ERROR)) {
        success = 0;
        goto cleanup;
    }
    fprintf(stderr, "PASS: Windows bank snapshot persistence and recovery\n");

cleanup:
    if (changed_directory && !SetCurrentDirectoryA(original_directory)) {
        fprintf(stderr, "FAIL: restore working directory (error %lu)\n", GetLastError());
        success = 0;
    }
    if (utilities_created) {
        char data_path[MAX_PATH];
        char temporary_path[MAX_PATH];
        char log_path[MAX_PATH];
        int data_length = snprintf(data_path, sizeof(data_path),
                                   "%s\\utilities\\bank.dat", temporary_directory);
        int temporary_length = snprintf(temporary_path, sizeof(temporary_path),
                                        "%s\\utilities\\bank.dat.tmp", temporary_directory);
        int log_length = snprintf(log_path, sizeof(log_path),
                                  "%s\\utilities\\transactions.log", temporary_directory);
        if (data_length < 0 || data_length >= (int)sizeof(data_path) ||
            temporary_length < 0 || temporary_length >= (int)sizeof(temporary_path) ||
            log_length < 0 || log_length >= (int)sizeof(log_path)) {
            fprintf(stderr, "FAIL: temporary persistence path exceeds MAX_PATH\n");
            success = 0;
        } else {
            if (!delete_file_if_present(data_path)) {
                success = 0;
            }
            if (!delete_file_if_present(temporary_path)) {
                success = 0;
            }
            if (!delete_file_if_present(log_path)) {
                success = 0;
            }
        }
        int utilities_length = snprintf(data_path, sizeof(data_path),
                                        "%s\\utilities", temporary_directory);
        if (utilities_length < 0 || utilities_length >= (int)sizeof(data_path)) {
            fprintf(stderr, "FAIL: temporary utilities path exceeds MAX_PATH\n");
            success = 0;
        } else if (!remove_directory_if_present(data_path)) {
            success = 0;
        }
    }
    if (!remove_directory_if_present(temporary_directory)) {
        success = 0;
    }
    return success;
}

static DWORD WINAPI authentication_worker(LPVOID argument) {
    const worker_argument_t *worker = (const worker_argument_t *)argument;
    server_ctx_t context;
    session_t session;
    client_id_t client_id;
    memset(&context, 0, sizeof(context));
    memset(&session, 0, sizeof(session));
    context.bank = worker->bank;
    context.worker_id = (int)worker->worker_index + 1;
    context.pid = GetCurrentProcessId();
    registry_make_id(&client_id, worker->worker_index % ACCOUNT_COUNT);

    /* Repeated concurrent logins exercise the atomic session-token counter. */
    for (unsigned i = 0; i < worker->iterations; i++) {
        if (auth_login(&context, &session, client_id.value, GetCurrentProcessId()) != STATUS_OK ||
            auth_check(&session, client_id.value, session.session_id.value) != STATUS_OK ||
            registry_find(worker->bank, &client_id) == NULL) {
            return 1;
        }
        auth_logout(&session);
        if (auth_check(&session, client_id.value, "") != STATUS_SESSION_INVALID) {
            return 1;
        }
    }
    return 0;
}

static ULONGLONG file_time_value(FILETIME value) {
    ULARGE_INTEGER combined;
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

static int run_concurrent_stress(bank_t *bank, unsigned worker_count, unsigned iterations) {
    HANDLE workers[MAX_TEST_WORKERS];
    worker_argument_t arguments[MAX_TEST_WORKERS];
    unsigned started = 0;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER finish;
    FILETIME creation_before;
    FILETIME exit_before;
    FILETIME kernel_before;
    FILETIME user_before;
    FILETIME creation_after;
    FILETIME exit_after;
    FILETIME kernel_after;
    FILETIME user_after;
    PROCESS_MEMORY_COUNTERS_EX memory_before;
    PROCESS_MEMORY_COUNTERS_EX memory_after;

    memset(&memory_before, 0, sizeof(memory_before));
    memory_before.cb = (DWORD)sizeof(memory_before);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&memory_before,
                              (DWORD)sizeof(memory_before)) ||
        !GetProcessTimes(GetCurrentProcess(), &creation_before, &exit_before,
                         &kernel_before, &user_before) ||
        !QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&start)) {
        return fail("collect initial Windows performance measurements");
    }

    for (unsigned i = 0; i < worker_count; i++) {
        arguments[i].bank = bank;
        arguments[i].worker_index = i;
        arguments[i].iterations = iterations;
        workers[i] = CreateThread(NULL, 0, authentication_worker, &arguments[i], 0, NULL);
        if (workers[i] == NULL) {
            fprintf(stderr, "FAIL: CreateThread worker %u (error %lu)\n", i, GetLastError());
            break;
        }
        started++;
    }

    int success = (started == worker_count);
    if (started != 0) {
        DWORD wait_result = WaitForMultipleObjects(started, workers, TRUE, INFINITE);
        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "FAIL: wait for stress workers (error %lu)\n", GetLastError());
            success = 0;
            for (unsigned i = 0; i < started; i++) {
                if (WaitForSingleObject(workers[i], INFINITE) != WAIT_OBJECT_0) {
                    fprintf(stderr, "FAIL: wait for stress worker %u (error %lu)\n",
                            i, GetLastError());
                    success = 0;
                }
            }
        }
    }
    for (unsigned i = 0; i < started; i++) {
        DWORD exit_code = 1;
        if (!GetExitCodeThread(workers[i], &exit_code) || exit_code != 0) {
            fprintf(stderr, "FAIL: authentication stress worker %u\n", i);
            success = 0;
        }
        CloseHandle(workers[i]);
    }

    memset(&memory_after, 0, sizeof(memory_after));
    memory_after.cb = (DWORD)sizeof(memory_after);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&memory_after,
                              (DWORD)sizeof(memory_after)) ||
        !GetProcessTimes(GetCurrentProcess(), &creation_after, &exit_after,
                         &kernel_after, &user_after) ||
        !QueryPerformanceCounter(&finish)) {
        return fail("collect final Windows performance measurements");
    }
    if (!success) {
        return 0;
    }

    double elapsed_ms = (double)(finish.QuadPart - start.QuadPart) * 1000.0 /
                        (double)frequency.QuadPart;
    double user_ms = (double)(file_time_value(user_after) - file_time_value(user_before)) / 10000.0;
    double kernel_ms = (double)(file_time_value(kernel_after) - file_time_value(kernel_before)) / 10000.0;
    printf("%u,%u,%" PRIu64 ",%.3f,%.3f,%.3f,%zu,%zu,%zu\n",
           worker_count, iterations,
           (uint64_t)worker_count * (uint64_t)iterations,
           elapsed_ms, user_ms, kernel_ms,
           (size_t)memory_before.WorkingSetSize / 1024U,
           (size_t)memory_after.WorkingSetSize / 1024U,
           (size_t)memory_after.PeakWorkingSetSize / 1024U);
    fprintf(stderr, "PASS: %u threads completed %u authentication cycles each\n",
            worker_count, iterations);
    return 1;
}

static int parse_argument(const char *text, unsigned maximum, unsigned *value) {
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (text[0] == '\0' || end == text || *end != '\0' || parsed == 0 || parsed > maximum) {
        return 0;
    }
    *value = (unsigned)parsed;
    return 1;
}

int main(int argc, char **argv) {
    unsigned workers = DEFAULT_WORKERS;
    unsigned iterations = DEFAULT_ITERATIONS;
    if ((argc > 1 && !parse_argument(argv[1], MAX_TEST_WORKERS, &workers)) ||
        (argc > 2 && !parse_argument(argv[2], MAX_TEST_ITERATIONS, &iterations)) ||
        argc > 3) {
        fprintf(stderr, "Usage: %s [workers: 1-%u] [iterations-per-worker: 1-%u]\n",
                argv[0], MAX_TEST_WORKERS, MAX_TEST_ITERATIONS);
        return 2;
    }

    bank_t bank;
    if (!test_registry(&bank) || !test_authentication(&bank) || !test_persistence()) {
        return 1;
    }
    puts("workers,iterations_per_worker,auth_cycles,elapsed_ms,process_user_ms,process_kernel_ms,working_set_before_kb,working_set_after_kb,peak_working_set_kb");
    return run_concurrent_stress(&bank, workers, iterations) ? 0 : 1;
}
