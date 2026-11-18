@echo off
rem Builds build\server.exe and build\client.exe with MinGW gcc.
rem Run from Win\ipc.

set CFLAGS=-std=c99 -Wall -Wextra -O2 -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1
set COMMON=named_pipe/named_pipe.c utilities/status_string.c

if not exist build mkdir build

gcc %CFLAGS% -o build/server.exe server/main.c server/server.c server/authentication.c server/client_registry.c server/transaction.c server/persistence.c server/operator.c shared_memory/shared_memory.c synchronization/semaphore.c %COMMON%
if errorlevel 1 exit /b 1

gcc %CFLAGS% -o build/client.exe client/main.c client/runner.c client/session.c %COMMON%
if errorlevel 1 exit /b 1

echo built build\server.exe and build\client.exe
