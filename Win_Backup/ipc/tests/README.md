# Windows transaction tests and measurements

Run the C regression test from a Visual Studio Developer Command Prompt:

```bat
tests\run_tests.bat
```

The test uses uniquely named Windows shared-memory and mutex objects and an
isolated temporary working directory, so it does not use or overwrite a live
server's bank file or IPC objects. It checks account initialization, login and
logout, balance queries, zero/negative amounts, successful deposits and
withdrawals, overdrafts, transfers, self-transfer rejection, invalid
recipients, duplicate-request idempotency, operator-approved and rejected
deposits, balance overflow, snapshot save/load/replacement, and truncated-file
rejection. A multithreaded stress pass verifies the exact final balances and
the bank accounting invariant.

Optional arguments choose worker threads, deposits per thread, and the sample
number:

```bat
tests\transaction_stress_test.exe 8 10000 1
```

Allowed ranges are 1–16 threads, 1–100,000 deposits per thread, and sample
number 1–100. Each successful run prints one CSV row. Windows does not provide
Linux `getrusage`, `/proc`, or `vmstat`; the C test collects equivalent
platform-appropriate process CPU time, page-fault counts, working-set memory,
system available physical memory, and elapsed time through Win32 performance
APIs.

To run a 1/2/4/8-thread sweep, repeat each sample, and create two SVG graphs:

```bat
set CROSSIPC_BENCHMARK=1
set CROSSIPC_ITERATIONS=10000
set CROSSIPC_REPEATS=3
tests\run_tests.bat
```

The CSV and SVG graphs are written to `tests\results`. The runner force-includes
`windows_sdk_compat.h` so Windows NTSTATUS macros do not collide with the
project's `STATUS_TIMEOUT` and `STATUS_INTERRUPTED` enum names.

Suggested commit message:

```text
test(windows): add transaction stress tests and resource measurements
```
