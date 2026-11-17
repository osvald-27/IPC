#include "semaphore.h"
#include <stdlib.h>
#include <string.h>
status_t semaphore_create(const char *name,transaction_lock_t **lock) {
if(!name||!lock)return STATUS_INVALID_ARGUMENT;
transaction_lock_t *item=calloc(1,sizeof(*item));
if(!item)return STATUS_OUT_OF_MEMORY;
strncpy(item->name,name,sizeof(item->name)-1);
item->handle=CreateSemaphoreA(NULL,1,1,name);
if(!item->handle){free(item);return STATUS_SYNCHRONIZATION_ERROR;}
if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(item->handle);free(item);return STATUS_ALREADY_EXISTS;}
*lock=item;
return STATUS_OK;
}
status_t semaphore_open(const char *name,transaction_lock_t **lock) {
if(!name||!lock)return STATUS_INVALID_ARGUMENT;
transaction_lock_t *item=calloc(1,sizeof(*item));
if(!item)return STATUS_OUT_OF_MEMORY;
strncpy(item->name,name,sizeof(item->name)-1);
item->handle=OpenSemaphoreA(SYNCHRONIZE|SEMAPHORE_MODIFY_STATE,FALSE,name);
if(!item->handle){free(item);return STATUS_SYNCHRONIZATION_ERROR;}
*lock=item;
return STATUS_OK;
}
status_t semaphore_lock(transaction_lock_t *lock) {
if(!lock||!lock->handle)return STATUS_INVALID_ARGUMENT;
DWORD result=WaitForSingleObject(lock->handle,INFINITE);
return result==WAIT_OBJECT_0?STATUS_OK:STATUS_SYNCHRONIZATION_ERROR;
}
status_t semaphore_unlock(transaction_lock_t *lock) {
if(!lock||!lock->handle)return STATUS_INVALID_ARGUMENT;
return ReleaseSemaphore(lock->handle,1,NULL)?STATUS_OK:STATUS_SYNCHRONIZATION_ERROR;
}
void semaphore_remove(transaction_lock_t *lock) {
if(!lock)return;
if(lock->handle)CloseHandle(lock->handle);
free(lock);
}
