set datafile separator ","
set terminal pngcairo size 1200,700
set xlabel "Worker processes"
set grid

set output "tests/results/throughput.png"
set ylabel "Transactions per second"
plot "tests/results/measurements.csv" using 2:(1000.0 * $4 / $5) smooth unique \
    with linespoints linewidth 2 pointtype 7 title "Locked transaction throughput"

set output "tests/results/child_memory.png"
set ylabel "Peak child RSS (KiB)"
plot "tests/results/measurements.csv" using 2:8 smooth unique \
    with linespoints linewidth 2 pointtype 7 title "getrusage(RUSAGE_CHILDREN)"
