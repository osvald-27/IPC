@echo off
setlocal
cd /d "%~dp0\.."

where cl >nul 2>nul
if errorlevel 1 (
    echo Microsoft C compiler not found. Run this from a Visual Studio Developer Command Prompt. 1>&2
    exit /b 1
)

if not exist tests\results mkdir tests\results

cl /nologo /std:c11 /W4 /D_CRT_SECURE_NO_WARNINGS ^
    /FI"%~dp0windows_sdk_compat.h" ^
    tests\transaction_stress_test.c ^
    server\transaction.c ^
    server\client_registry.c ^
    server\authentication.c ^
    server\persistence.c ^
    synchronization\semaphore.c ^
    synchronization\txlock.c ^
    shared_memory\shared_memory.c ^
    /Fe:tests\transaction_stress_test.exe ^
    /link Psapi.lib
if errorlevel 1 exit /b 1

if "%CROSSIPC_BENCHMARK%"=="1" goto benchmark
tests\transaction_stress_test.exe %*
exit /b %errorlevel%

:benchmark
if not defined CROSSIPC_ITERATIONS set CROSSIPC_ITERATIONS=1000
if not defined CROSSIPC_REPEATS set CROSSIPC_REPEATS=3
echo repeat,workers,iterations_per_worker,transactions,elapsed_ms,process_user_ms,process_kernel_ms,working_set_before_kb,working_set_after_kb,peak_working_set_kb,page_faults_before,page_faults_after,system_available_physical_kb> tests\results\measurements.csv
for %%W in (1 2 4 8) do (
    for /L %%R in (1,1,%CROSSIPC_REPEATS%) do (
        echo Running repeat %%R with %%W worker threads... 1>&2
        tests\transaction_stress_test.exe %%W %CROSSIPC_ITERATIONS% %%R > tests\results\sample-output.tmp
        if errorlevel 1 exit /b 1
        findstr /r /c:"^[0-9][0-9]*,[0-9][0-9]*,[0-9][0-9]*,[0-9][0-9]*," tests\results\sample-output.tmp >> tests\results\measurements.csv
        if errorlevel 1 exit /b 1
        del /q tests\results\sample-output.tmp
    )
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\plot_measurements.ps1 tests\results\measurements.csv tests\results
if errorlevel 1 exit /b 1
echo Measurement results written to tests\results
exit /b 0
