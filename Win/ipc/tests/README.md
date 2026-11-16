# Windows C tests

Run the C regression and stress tests from a Visual Studio Developer Command
Prompt:

```bat
tests\run_tests.bat
```

The test binary checks Windows account initialization and validation,
accounting invariants, authentication/session lifecycle, bank snapshot
save/load/replacement and truncated-file rejection. It then runs concurrent
authentication and registry lookups using Windows threads and prints a CSV
measurement row with elapsed time, process CPU time, and working-set memory.

Optional arguments set the worker count and iterations per worker:

```bat
tests\run_tests.bat 8 10000
```

Defaults are 8 workers and 5,000 iterations per worker. Valid ranges are
1–16 workers and 1–200,000 iterations. The tests use an isolated temporary
directory for persistence data and do not touch the server's real bank files.

These tests cover the Windows registry, authentication, and persistence
components. Named-pipe, shared-memory, semaphore, and client transport source
files are still empty in this checkout, so this harness does not claim
end-to-end coverage of those IPC paths.

Suggested commit message:

```text
test(windows): add registry and authentication stress tests
```
