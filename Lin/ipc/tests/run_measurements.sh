#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

if ! command -v vmstat >/dev/null 2>&1; then
    echo "vmstat is required; install the procps package and retry." >&2
    exit 1
fi

make tests/transaction_stress_test
mkdir -p tests/results

csv_file="tests/results/measurements.csv"
vmstat_file="tests/results/vmstat.log"
run_output="tests/results/current-run.log"
printf '%s\n' \
    'repeat,workers,iterations_per_worker,transactions,elapsed_ms,child_user_ms,child_system_ms,child_maxrss_kb,child_minor_faults,child_major_faults,child_voluntary_context_switches,child_involuntary_context_switches,proc_vmsize_before_kb,proc_rss_before_kb,proc_vmpeak_after_kb,proc_vmsize_after_kb,proc_rss_after_kb' \
    > "$csv_file"

# Keep a system-wide sample running alongside each workload for vmstat comparison.
vmstat 1 > "$vmstat_file" &
vmstat_pid=$!
cleanup() {
    kill "$vmstat_pid" 2>/dev/null || true
    wait "$vmstat_pid" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

iterations=${CROSSIPC_ITERATIONS:-1000}
repeats=${CROSSIPC_REPEATS:-3}
case "$repeats" in
    ''|*[!0-9]*)
        echo "CROSSIPC_REPEATS must be between 1 and 100." >&2
        exit 2
        ;;
esac
if [ "$repeats" -lt 1 ] || [ "$repeats" -gt 100 ]; then
    echo "CROSSIPC_REPEATS must be between 1 and 100." >&2
    exit 2
fi

for workers in 1 2 4 8; do
    repeat=1
    while [ "$repeat" -le "$repeats" ]; do
        if ! ./tests/transaction_stress_test "$workers" "$iterations" > "$run_output"; then
            cat "$run_output" >&2
            exit 1
        fi
        row=$(tail -n 1 "$run_output")
        case "$row" in
            *,*) ;;
            *)
                echo "Stress test did not produce a CSV measurement row." >&2
                exit 1
                ;;
        esac
        printf '%s,%s\n' "$repeat" "$row" >> "$csv_file"
        repeat=$((repeat + 1))
    done
done

if command -v gnuplot >/dev/null 2>&1; then
    gnuplot tests/plot_measurements.gp
else
    echo "gnuplot not installed; CSV and vmstat data were saved without plots." >&2
fi

echo "Measurements: $csv_file"
echo "System samples: $vmstat_file"
