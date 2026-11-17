#include "shared_memory.h"
#include <string.h>
status_t shared_memory_create(const char *name,HANDLE *handle,bank_t **bank) {
if(!name||!handle||!bank)return STATUS_INVALID_ARGUMENT;
HANDLE mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)sizeof(bank_t),name);
if(!mapping)return STATUS_SHARED_MEMORY_ERROR;
DWORD error=GetLastError();
void *address=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(bank_t));
if(!address){CloseHandle(mapping);return STATUS_SHARED_MEMORY_ERROR;}
if(error!=ERROR_ALREADY_EXISTS)memset(address,0,sizeof(bank_t));
*handle=mapping;
*bank=(bank_t*)address;
return STATUS_OK;
}
status_t shared_memory_open(const char *name,HANDLE *handle,bank_t **bank) {
if(!name||!handle||!bank)return STATUS_INVALID_ARGUMENT;
HANDLE mapping=OpenFileMappingA(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name);
if(!mapping)return STATUS_SHARED_MEMORY_ERROR;
void *address=MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(bank_t));
if(!address){CloseHandle(mapping);return STATUS_SHARED_MEMORY_ERROR;}
*handle=mapping;
*bank=(bank_t*)address;
return STATUS_OK;
}
void shared_memory_close(HANDLE handle,bank_t *bank) {
if(bank)UnmapViewOfFile(bank);
if(handle)CloseHandle(handle);
}
