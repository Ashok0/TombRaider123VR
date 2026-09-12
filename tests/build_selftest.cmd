@echo off
REM Builds and runs the self-test. Needs a VS x64 developer environment; this
REM script finds one via vswhere if cl.exe is not already on PATH.
REM
REM No parenthesised blocks below on purpose: %ProgramFiles(x86)% contains
REM parentheses and breaks cmd's block parsing even when quoted.
setlocal

where cl.exe >nul 2>&1
if not errorlevel 1 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs

REM Route through a temp file: a quoted exe path inside a for/f backquote is
REM fragile, and this path always has parentheses in it.
"%VSWHERE%" -latest -products * -property installationPath > "%TEMP%\_trvr_vspath.txt" 2>nul
set "VSPATH="
set /p VSPATH=<"%TEMP%\_trvr_vspath.txt"
del "%TEMP%\_trvr_vspath.txt" >nul 2>&1
if not defined VSPATH goto no_vs
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl.exe >nul 2>&1
if errorlevel 1 goto no_vs

:have_cl
set "ROOT=%~dp0.."
if not exist "%ROOT%\build\test" mkdir "%ROOT%\build\test"
pushd "%ROOT%\build\test"

cl /nologo /std:c++17 /permissive- /EHsc /W4 /MT /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /I "%ROOT%\src" "%ROOT%\tests\selftest.cpp" "%ROOT%\src\InlineHook.cpp" "%ROOT%\src\Log.cpp" /Fe:selftest.exe
if errorlevel 1 goto build_failed

.\selftest.exe
set RC=%ERRORLEVEL%
popd
exit /b %RC%

:build_failed
popd
echo Build failed.
exit /b 1

:no_vs
echo Could not locate a Visual Studio x64 toolchain. Run from a developer prompt.
exit /b 1
