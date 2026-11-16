# CrossIPC Windows implementation (MinGW-w64)

This is the Windows counterpart of `Lin/ipc`. The banking rules and persisted bank-state layout are kept aligned with Linux. Only the operating-system IPC adapters differ:

| Linux | Windows |
|---|---|
| System V message queue | Windows Named Pipe (`\\.\pipe\CrossIPC_Bank`) |
| System V shared memory | Windows file mapping (`Local\CrossIPC_BankSharedMemory`) |
| System V semaphore | Named Windows mutex (`Local\CrossIPC_BankMutex`) |
| `fork()` workers | Win32 worker threads |

The Windows `Message` omits Linux's leading System V `mtype` field because Named Pipes route data over a connection rather than by message-queue type. The remaining request fields and banking operations are retained.

## Requirements

- 64-bit Windows 10/11
- 64-bit MinGW-w64 GCC toolchain
- GNU Make (`mingw32-make`)

Use the x86_64 MinGW-w64 toolchain so the compile-time layout checks for the shared persisted bank structure are enabled.

## Build

Open the MinGW-w64 terminal and run:

```sh
cd IPC-main/Win/ipc
mingw32-make clean
mingw32-make
```

This produces:

- `server_app.exe`
- `client_app.exe`

## Run

Open one terminal for the server:

```sh
cd IPC-main/Win/ipc
./server_app.exe
```

Open another terminal for a client:

```sh
cd IPC-main/Win/ipc
./client_app.exe ACC0001
```

The seeded accounts are `ACC0001`, `ACC0002`, and `ACC0003`; each starts with 100,000 XAF when a new bank file is created.

Client commands:

```text
balance
deposit 1000
withdraw 500
transfer ACC0002 250
status
help
logout
```

For automated deposit tests, start the server with `--auto-approve`. Without that option, deposits enter the operator approval queue; use `pending`, `approve <TxnID>`, or `reject <TxnID>` in the server console.

Useful server options:

```text
--workers N       Worker thread count (1..32)
--fresh           Ignore saved bank.dat and initialize new accounts
--nolock          Disable the transaction lock for the lost-update demonstration
--auto-approve    Approve deposits immediately
--help            Show options
```

## Persistence compatibility

On 64-bit builds, `server.h` contains compile-time assertions for the sizes of `account_t`, `pending_deposit_t`, `idempotency_entry_t`, `session_entry_t`, and `bank_t`. The expected `bank_t` size is 19,616 bytes, matching the Linux x86-64 implementation. The shared common headers (`types.h`, `status.h`, `transaction.h`, and `protocol.h`) are copied from the Linux implementation.

The Windows and Linux builds write their data under their own `utilities` directories. To move a saved bank between platforms, stop the server first and copy `utilities/bank.dat`; do not copy it while either server is running.

## Basic verification sequence

1. Start the server and confirm the three seeded accounts are displayed.
2. Log in as `ACC0001` and run `balance`.
3. Deposit 1,000 XAF; approve it if the server is not in `--auto-approve` mode.
4. Withdraw 500 XAF.
5. Transfer 250 XAF to `ACC0002`.
6. Check balances from both accounts.
7. Try a withdrawal larger than the available balance; it must be rejected without changing the balance.
8. Try a transfer to `ACC0001` from `ACC0001`; it must be rejected as a self-transfer.
9. Stop the server normally, restart it, and confirm balances were restored.
10. Run concurrent clients and confirm the server's total-funds consistency report remains `CONSISTENT` with locking enabled.

## Verification limitation

The source was checked for C syntax and the Linux/Windows persisted bank structures were compared in this workspace. A MinGW-w64 compiler and Windows runtime were not available here, so the `.exe` files have **not** been compiled or executed in this environment. Run the build and verification sequence above on the target Windows machine before treating the Windows port as runtime-verified.
