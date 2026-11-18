# CrossIPC, Windows version

The bank service built on the Win32 API. Same rules as the Linux version: three accounts of 100,000 XAF, deposits, withdrawals and transfers, and the total must always add up.

## Build

Needs MinGW gcc. From `Win/ipc` in cmd or PowerShell:

    build.bat

or, if `mingw32-make` is installed:

    mingw32-make

This makes `build\server.exe` and `build\client.exe`.

## Run

Window 1, the server:

    build\server.exe --fresh

Window 2, a client:

    build\client.exe ACC0001

Client commands: `balance`, `deposit 500`, `withdraw 200`, `transfer ACC0002 300`, `check`, `quit`.
Server console commands: `status`, `quit`. Ctrl+C also stops the server cleanly.

Server options:

| Option | Meaning |
|---|---|
| `--workers N` | number of worker processes (default 8) |
| `--fresh` | start from the initial balances, ignore `bank.dat` |
| `--nolock` | run without the semaphore, to show a lost update |

## Throughput test

With the server running:

    tests\bench.bat

It starts 1, 2, 4 and 8 client processes with `CreateProcess()`, each sending 20000 random transactions, and writes `tests\windows_results.csv`. A transaction counts when the server commits it. Refusals such as insufficient funds are not counted.

## How it is built

| Part | Windows call | File |
|---|---|---|
| Worker and client processes | `CreateProcess`, `WaitForMultipleObjects`, `GetExitCodeProcess` | `server/main.c`, `client/main.c` |
| Requests and replies | `CreateNamedPipe`, `ConnectNamedPipe`, `CreateFile`, `ReadFile`, `WriteFile`, `DisconnectNamedPipe` | `named_pipe/named_pipe.c` |
| Account balances | `CreateFileMapping`, `OpenFileMapping`, `MapViewOfFile`, `UnmapViewOfFile` | `shared_memory/shared_memory.c` |
| Lock | `CreateSemaphore`, `OpenSemaphore`, `WaitForSingleObject`, `ReleaseSemaphore` | `synchronization/semaphore.c` |
| Ctrl+C | `SetConsoleCtrlHandler`, a named manual-reset event | `server/main.c`, `client/main.c` |
| Errors | `GetLastError` after every failed call | everywhere |

The server parent creates the shared memory, the semaphore and the shutdown event, then starts the workers. No handle is inherited: each worker opens the three objects by name. Each worker creates its own instance of the pipe and serves one client at a time, so 8 workers serve 8 clients at once. Because the workers are separate processes updating the same memory, the semaphore is needed. `--nolock` shows what happens without it.

Shutdown: Ctrl+C or `quit` sets the shutdown event. Every blocked pipe call is also waiting on that event, so the workers leave at once, close their handles and exit. The parent waits for them, prints the consistency check, saves `bank.dat` and closes its handles. Windows frees a named object when its last handle closes, so nothing is left behind.

## Known limits

- Login is by account id only. The password, Argon2id and AES-256-GCM part of the design document is not implemented.
- `bank.dat` is written at shutdown only. A crash of the whole server loses the changes since the last start.
- If a worker is killed while it holds the semaphore, the semaphore is never released. A Windows mutex would report this as `WAIT_ABANDONED`. A semaphore does not.
- `status.h` includes `<windows.h>` and undefines `STATUS_TIMEOUT`, because `winnt.h` has a macro with the same name.
