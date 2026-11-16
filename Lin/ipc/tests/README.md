# Linux test and measurement harness

The C stress test exercises the System V message-queue wrapper, then runs the
Linux transaction engine against real shared memory and semaphore objects. It
checks account initialization, amount validation, overdrafts, self-transfers,
unknown recipients, transfers, deposit approval/rejection, transaction
idempotency, overflow handling, and concurrent balance/accounting invariants.

Run the functional scenarios and the default four-process stress test from
`Lin/ipc`:

```sh
make test
```

Run a worker-count sweep and collect measurements:

```sh
make bench
```

`tests/run_measurements.sh` runs 1, 2, 4, and 8 worker processes, repeating
each worker-count measurement three times. Each test process uses
`getrusage(RUSAGE_CHILDREN)` for child CPU time, peak resident set
size, page faults, and context switches. The per-worker peak RSS is collected
when each stress child exits. The harness also reads `VmPeak`, `VmSize`, and
`VmRSS` from `/proc/self/status`. `vmstat 1` records system-wide activity while
the sweep runs. Set `CROSSIPC_ITERATIONS` to change the number of deposits per
worker (default: 1000) or `CROSSIPC_REPEATS` to change the repetition count
(default: 3; maximum: 100). A test invocation accepts 1–16 processes and
1–100,000 deposits per process.

The runner writes raw repeated samples to `tests/results/measurements.csv` and
`tests/results/vmstat.log`. If gnuplot is installed, it also creates throughput
and peak-RSS PNG graphs in `tests/results`; each graph averages samples with
the same worker count. The result directory is generated output and should not
be committed.

Suggested commit message:

```text
test(linux): add IPC transaction stress and resource measurements
```
