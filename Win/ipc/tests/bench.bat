@echo off
rem Throughput test with 1, 2, 4 and 8 client processes.
rem Start the server first, in another window:   build\server.exe --fresh
rem Then run this from Win\ipc:                   tests\bench.bat
rem Results go to tests\windows_results.csv

set COUNT=20000
echo tag,clients,committed,seconds,tps> tests\windows_results.csv
for %%n in (1 2 4 8) do build\client.exe --spawn %%n %COUNT% 2>> tests\windows_results.csv
type tests\windows_results.csv
