@echo off
rem Run from a VS x64 developer prompt. Only physical-switch tests execute.
set "SWITCH_ROOT=%~dp0.."
if not exist "%SWITCH_ROOT%\build\switch-hand-reach-test" mkdir "%SWITCH_ROOT%\build\switch-hand-reach-test"
cl /nologo /std:c++17 /O2 /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%SWITCH_ROOT%\src" /I"%SWITCH_ROOT%\third_party\openvr\headers" "%SWITCH_ROOT%\tests\switch_reach_selftest.cpp" "%SWITCH_ROOT%\src\InlineHook.cpp" "%SWITCH_ROOT%\src\Log.cpp" /Fe"%SWITCH_ROOT%\build\switch-hand-reach-test\switch_reach_test.exe" /Fo"%SWITCH_ROOT%\build\switch-hand-reach-test\\" /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
"%SWITCH_ROOT%\build\switch-hand-reach-test\switch_reach_test.exe"
