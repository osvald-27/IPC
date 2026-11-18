@echo off
setlocal
cd /d "%~dp0\.."

where cl >nul 2>nul
if errorlevel 1 (
    echo Microsoft C compiler not found. Run this from a Visual Studio Developer Command Prompt. 1>&2
    exit /b 1
)

cl /nologo /std:c11 /W4 /D_CRT_SECURE_NO_WARNINGS ^
    tests\windows_ipc_stress_test.c ^
    server\client_registry.c ^
    server\authentication.c ^
    server\persistence.c ^
    /Fe:tests\windows_ipc_stress_test.exe ^
    /link Psapi.lib
if errorlevel 1 exit /b %errorlevel%

tests\windows_ipc_stress_test.exe %*
exit /b %errorlevel%
