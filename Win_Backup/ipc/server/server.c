#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void server_handle_request(server_ctx_t *ctx, session_t *session, const Message *request, Message *response) {
    memset(response, 0, sizeof(*response));
    response->request_id=request->request_id;
    response->sender_pid=(uint32_t)GetCurrentProcessId();
    response->operation=request->operation;
    snprintf(response->client_id,sizeof(response->client_id),"%.*s",CLIENT_ID_LEN-1,request->client_id);
    snprintf(response->session_id,sizeof(response->session_id),"%.*s",SESSION_ID_LEN-1,request->session_id);
    if (request->operation == MSG_LOGIN) {
        status_t st=auth_login(ctx,session,request->client_id,request->sender_pid);
        response->status_code=st;
        if (st==STATUS_OK) snprintf(response->session_id,sizeof(response->session_id),"%s",session->session_id.value);
        return;
    }
    status_t auth=auth_check(ctx,request->client_id,request->session_id);
    if (auth!=STATUS_OK) { response->status_code=auth; return; }
    switch (request->operation) {
    case MSG_BALANCE: {
        client_id_t id; balance_response_t balance; memset(&id,0,sizeof(id));
        snprintf(id.value,sizeof(id.value),"%.*s",(int)CLIENT_ID_LENGTH,request->client_id);
        status_t st=transaction_balance(ctx,&id,&balance); response->status_code=st;
        if (st==STATUS_OK) response->amount=balance.balance;
        break;
    }
    case MSG_DEPOSIT: case MSG_WITHDRAW: case MSG_TRANSFER:
        transaction_execute(ctx,request,response); break;
    case MSG_STATUS_QUERY:
        transaction_status_query(ctx,request->request_id,response); break;
    case MSG_SERVER_STATUS: {
        int consistent=0;
        if (ctx->use_lock && semaphore_lock(ctx->lock)!=STATUS_OK) { response->status_code=STATUS_SYNCHRONIZATION_ERROR; break; }
        consistent=registry_consistent(ctx->bank,NULL,NULL);
        if (ctx->use_lock) semaphore_unlock(ctx->lock);
        response->status_code=consistent?STATUS_OK:STATUS_INTERNAL_ERROR; break;
    }
    case MSG_LOGOUT:
        auth_logout(ctx,request->session_id); response->status_code=STATUS_OK;
        if (session) { memset(session, 0, sizeof(*session)); }
        break;
    default: response->status_code=STATUS_INVALID_ARGUMENT; break;
    }
}

static int pipe_read_all(HANDLE pipe, void *buffer, DWORD length) {
    BYTE *p=(BYTE *)buffer; DWORD done=0;
    while (done<length) { DWORD n=0; if (!ReadFile(pipe,p+done,length-done,&n,NULL)||n==0) return -1; done+=n; }
    return 0;
}
static int pipe_write_all(HANDLE pipe,const void *buffer,DWORD length) {
    const BYTE *p=(const BYTE *)buffer; DWORD done=0;
    while(done<length) { DWORD n=0; if(!WriteFile(pipe,p+done,length-done,&n,NULL)||n==0) return -1; done+=n; }
    return 0;
}
int server_run(server_ctx_t *ctx) {
    if (!ctx) return 1;
    while (!g_server_stop) {
        HANDLE pipe=CreateNamedPipeA(PIPE_NAME,PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,PIPE_UNLIMITED_INSTANCES,
            (DWORD)sizeof(Message),(DWORD)sizeof(Message),0,NULL);
        if (pipe==INVALID_HANDLE_VALUE) { Sleep(100); continue; }
        BOOL connected=ConnectNamedPipe(pipe,NULL)?TRUE:(GetLastError()==ERROR_PIPE_CONNECTED);
        if (!connected || g_server_stop) { CloseHandle(pipe); continue; }
        session_t session; memset(&session,0,sizeof(session));
        while (!g_server_stop) {
            Message request,response;
            if (pipe_read_all(pipe,&request,(DWORD)sizeof(request))!=0) break;
            server_handle_request(ctx,&session,&request,&response);
            if (pipe_write_all(pipe,&response,(DWORD)sizeof(response))!=0) break;
        }
        if (session.logged_in) auth_logout(ctx,session.session_id.value);
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
    }
    return 0;
}
