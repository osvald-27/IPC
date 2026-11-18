#include "server.h"
#include "../shared_memory/shared_memory.h"
#include <direct.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile LONG g_server_stop = 0;
static HANDLE g_workers[MAX_WORKERS];
static int g_worker_count=0;
static BOOL WINAPI console_handler(DWORD type) {
    if (type==CTRL_C_EVENT || type==CTRL_BREAK_EVENT || type==CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_server_stop,1); return TRUE;
    }
    return FALSE;
}
static DWORD WINAPI worker_entry(LPVOID arg) {
    server_ctx_t *ctx=(server_ctx_t *)arg;
    int result=server_run(ctx);
    free(ctx);
    return (DWORD)result;
}
static void wake_workers(void) {
    for (int i=0;i<g_worker_count;++i) {
        HANDLE h=CreateFileA(PIPE_NAME,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
        if (h!=INVALID_HANDLE_VALUE) CloseHandle(h);
    }
}
int main(int argc,char **argv) {
    int workers=DEFAULT_WORKERS,fresh=0,use_lock=1,auto_approve=0;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--workers")&&i+1<argc) workers=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--fresh")) fresh=1;
        else if(!strcmp(argv[i],"--nolock")) use_lock=0;
        else if(!strcmp(argv[i],"--auto-approve")) auto_approve=1;
        else if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")) {
            printf("Usage: %s [--workers N] [--fresh] [--nolock] [--auto-approve]\n",argv[0]); return 0;
        } else { fprintf(stderr,"Unknown argument: %s\n",argv[i]); return 2; }
    }
    if(workers<1||workers>MAX_WORKERS){fprintf(stderr,"--workers must be 1..%d\n",MAX_WORKERS);return 2;}
    SetConsoleCtrlHandler(console_handler,TRUE); _mkdir("utilities");
    server_ctx_t ctx; memset(&ctx,0,sizeof(ctx)); ctx.use_lock=use_lock; ctx.auto_approve=auto_approve; ctx.pid=GetCurrentProcessId();
    puts("=====================================================\n   CrossIPC Windows Banking Server (MinGW-w64)\n=====================================================");
    ShmRegion bank_region;
    ShmStatus shm_status = shm_create(SHM_NAME, sizeof(bank_t), &bank_region);
    if (shm_status == SHM_EXISTS) {
        fprintf(stderr, "A CrossIPC server is already running (shared mapping exists).\n");
        return 1;
    }
    if (shm_status != SHM_OK) {
        fprintf(stderr, "Unable to create shared bank mapping.\n");
        return 1;
    }
    ctx.bank = (bank_t *)bank_region.view;
    registry_init(ctx.bank);
    if(!fresh && persistence_load(ctx.bank)==STATUS_OK) printf("[STORAGE] Loaded %s\n",DATA_FILE);
    else printf("[STORAGE] Initialized accounts at %" PRId64 " %s each\n",INITIAL_BALANCE,CURRENCY_CODE);
    status_t lock_st=semaphore_create(SEM_NAME,&ctx.lock);
    if(lock_st==STATUS_ALREADY_EXISTS) lock_st=semaphore_open(SEM_NAME,&ctx.lock);
    if(lock_st!=STATUS_OK){fprintf(stderr,"Lock initialization failed: %s\n",server_status_string(lock_st));shm_close(&bank_region);return 1;}
    printf("[IPC] Named pipe: %s\n[IPC] Shared memory: %s\n[IPC] Mutex: %s\n",PIPE_NAME,SHM_NAME,use_lock?"enabled":"disabled (--nolock)");
    printf("[DEPOSITS] %s\n",auto_approve?"Automatic approval":"Operator approval required");
    for(int i=0;i<workers;++i){
        server_ctx_t *w=(server_ctx_t *)malloc(sizeof(*w)); if(!w) break;
        *w=ctx; w->worker_id=i+1;
        HANDLE th=CreateThread(NULL,0,worker_entry,w,0,NULL);
        if(!th){free(w);fprintf(stderr,"CreateThread failed: %lu\n",GetLastError());break;}
        g_workers[g_worker_count++]=th;
    }
    if(!g_worker_count){fprintf(stderr,"No workers started.\n");InterlockedExchange(&g_server_stop,1);}
    printf("[SERVER] %d worker(s) running. Commands: status, pending, approve <id>, reject <id>, quit\n",g_worker_count);
    operator_print_status(&ctx); operator_start(&ctx);
    while(!g_server_stop) Sleep(100);
    puts("\n[SERVER] Shutting down..."); operator_stop(); wake_workers();
    /* Cancel workers blocked in synchronous ConnectNamedPipe/ReadFile before releasing shared memory. */
    for (int i=0; i<g_worker_count; ++i) CancelSynchronousIo(g_workers[i]);
    if(g_worker_count) WaitForMultipleObjects((DWORD)g_worker_count,g_workers,TRUE,INFINITE);
    for(int i=0;i<g_worker_count;++i) CloseHandle(g_workers[i]);
    operator_print_status(&ctx);
    if(persistence_save(ctx.bank)==STATUS_OK) printf("[STORAGE] Saved %s\n",DATA_FILE);
    semaphore_remove(ctx.lock); shm_close(&bank_region);
    puts("[SERVER] Shutdown complete."); return 0;
}
