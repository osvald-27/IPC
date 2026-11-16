#define _POSIX_C_SOURCE 200809L

#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_shutdown = 0;
static pid_t g_workers[MAX_WORKERS];
static int g_num_workers = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

static void ensure_key_file(const char *path) {
    mkdir("utilities", 0755);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
    }
}

static int server_shm_init(const char *key_path, char proj_id, int *shm_id, bank_t **bank) {
    key_t key = ftok(key_path, proj_id);
    if (key == (key_t)-1) {
        key = ftok("ipc.key", proj_id);
    }
    int id = shmget(key, sizeof(bank_t), IPC_CREAT | 0600);
    if (id == -1) {
        perror("shmget");
        return -1;
    }
    void *addr = shmat(id, NULL, 0);
    if (addr == (void *)-1) {
        perror("shmat");
        return -1;
    }
    *shm_id = id;
    *bank = (bank_t *)addr;
    return 0;
}

static int server_mq_init(const char *key_path, char proj_id) {
    key_t key = ftok(key_path, proj_id);
    if (key == (key_t)-1) {
        key = ftok("ipc.key", proj_id);
    }
    int qid = msgget(key, IPC_CREAT | 0600);
    if (qid == -1) {
        perror("msgget");
        return -1;
    }
    return qid;
}

int main(int argc, char **argv) {
    int workers = DEFAULT_WORKERS;
    int fresh = 0;
    int use_lock = 1;
    int auto_approve = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fresh") == 0) {
            fresh = 1;
        } else if (strcmp(argv[i], "--nolock") == 0) {
            use_lock = 0;
        } else if (strcmp(argv[i], "--auto-approve") == 0) {
            auto_approve = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--workers N] [--fresh] [--nolock] [--auto-approve]\n", argv[0]);
            printf("  --workers N     Number of worker processes to spawn (default: %d, max: %d)\n",
                   DEFAULT_WORKERS, MAX_WORKERS);
            printf("  --fresh         Start with fresh accounts, ignoring saved bank.dat\n");
            printf("  --nolock        Disable semaphore lock to demonstrate lost-update flaw\n");
            printf("  --auto-approve  Automatically approve cash deposits (for tests/benchmarks)\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown argument '%s'. Use --help for usage.\n", argv[i]);
            return 2;
        }
    }

    if (workers < 1 || workers > MAX_WORKERS) {
        fprintf(stderr, "Error: --workers must be between 1 and %d\n", MAX_WORKERS);
        return 2;
    }

    /* Signal handlers for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    const char *key_path = IPC_KEY_FILE;
    ensure_key_file(key_path);

    server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.use_lock = use_lock;
    ctx.auto_approve = auto_approve;
    ctx.worker_id = 0;
    ctx.pid = getpid();

    printf("=====================================================\n");
    printf("   CrossIPC Linux Banking Server (CEF427 Group 10)  \n");
    printf("=====================================================\n");

    /* 1. Create System V Shared Memory */
    if (server_shm_init(key_path, SHM_PROJ_ID, &ctx.shm_id, &ctx.bank) != 0) {
        fprintf(stderr, "Failed to initialize shared memory\n");
        return 1;
    }

    /* 2. Initialize or recover account state */
    registry_init(ctx.bank);
    if (!fresh && persistence_load(ctx.bank) == STATUS_OK) {
        printf("[STORAGE] Account balances loaded from %s\n", DATA_FILE);
    } else {
        printf("[STORAGE] Initialized fresh accounts with %" PRId64 " %s each\n",
               INITIAL_BALANCE, CURRENCY_CODE);
    }

    /* 3. Create System V Semaphore */
    status_t sem_st = semaphore_create(key_path, SEM_PROJ_ID, &ctx.lock);
    if (sem_st != STATUS_OK) {
        /* If semaphore already exists from an earlier abnormal stop, open and reuse or clear */
        if (sem_st == STATUS_ALREADY_EXISTS) {
            semaphore_open(key_path, SEM_PROJ_ID, &ctx.lock);
        } else {
            fprintf(stderr, "Failed to create semaphore (%s)\n", server_status_string(sem_st));
            shmdt(ctx.bank);
            shmctl(ctx.shm_id, IPC_RMID, NULL);
            return 1;
        }
    }

    /* 4. Create System V Message Queue */
    ctx.msg_qid = server_mq_init(key_path, MSG_PROJ_ID);
    if (ctx.msg_qid == -1) {
        fprintf(stderr, "Failed to create message queue\n");
        semaphore_remove(ctx.lock);
        shmdt(ctx.bank);
        shmctl(ctx.shm_id, IPC_RMID, NULL);
        return 1;
    }

    printf("[IPC] Message Queue ID:   %d\n", ctx.msg_qid);
    printf("[IPC] Concurrency Lock:   %s\n", use_lock ? "ENABLED (System V Semaphore)" : "DISABLED (--nolock: Lost Update demonstration)");
    printf("[IPC] Deposit Approval:   %s\n", auto_approve ? "AUTO-APPROVE" : "OPERATOR REVIEW (approve/reject)");

    /* 5. Spawn Worker Processes */
    for (int i = 0; i < workers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork worker failed");
            break;
        }

        if (pid == 0) {
            /* Child Worker Process */
            server_ctx_t worker_ctx = ctx;
            worker_ctx.worker_id = i + 1;
            worker_ctx.pid = getpid();

            int code = worker_run(&worker_ctx);
            exit(code);
        }

        g_workers[g_num_workers++] = pid;
    }

    printf("[SERVER] Started %d worker processes successfully\n", g_num_workers);
    printf("[SERVER] Type 'status', 'pending', 'approve <TxnID>', 'quit', or press Ctrl+C\n");
    fflush(stdout);

    /* 6. Display initial status and start operator thread */
    operator_print_status(&ctx);
    operator_start(&ctx);

    /* 7. Server Main Loop: monitor workers & listen for shutdown */
    while (!g_shutdown && !ctx.stop) {
        int status;
        pid_t dead_child = waitpid(-1, &status, WNOHANG);
        if (dead_child > 0) {
            fprintf(stderr, "\n[WARNING] Worker process %ld exited unexpectedly\n", (long)dead_child);
            break;
        }
        struct timespec sleep_ts;
        sleep_ts.tv_sec = 0;
        sleep_ts.tv_nsec = 100000000L;  /* 100ms */
        nanosleep(&sleep_ts, NULL);
    }

    /* 8. Graceful Shutdown Sequence */
    printf("\n[SERVER] Initiating graceful shutdown...\n");
    fflush(stdout);
    ctx.stop = 1;
    operator_stop();

    /* Terminate child worker processes */
    for (int i = 0; i < g_num_workers; i++) {
        if (g_workers[i] > 0) {
            kill(g_workers[i], SIGTERM);
        }
    }

    /* Wait for workers to finish */
    for (int i = 0; i < g_num_workers; i++) {
        if (g_workers[i] > 0) {
            int status;
            waitpid(g_workers[i], &status, 0);
        }
    }

    /* Print final account consistency report */
    printf("\n[REPORT] Final Account State:\n");
    operator_print_status(&ctx);

    /* Save persistent state */
    if (persistence_save(ctx.bank) == STATUS_OK) {
        printf("[STORAGE] Account state saved to %s\n", DATA_FILE);
    }

    /* 9. Cleanup IPC Resources */
    msgctl(ctx.msg_qid, IPC_RMID, NULL);
    semaphore_remove(ctx.lock);
    shmdt(ctx.bank);
    shmctl(ctx.shm_id, IPC_RMID, NULL);

    printf("[SERVER] Clean shutdown complete. All System V IPC resources released.\n");
    return 0;
}
