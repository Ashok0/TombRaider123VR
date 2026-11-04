@echo off
rem Run from a VS x64 developer prompt. This runs TR1-3 jump arm continuity checks.
set "TRANSITION_ROOT=%~dp0.."
if not exist "%TRANSITION_ROOT%\build\jump-arms-test" mkdir "%TRANSITION_ROOT%\build\jump-arms-test"
cl /nologo /std:c++17 /O2 /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%TRANSITION_ROOT%\src" /I"%TRANSITION_ROOT%\third_party\openvr\headers" "%TRANSITION_ROOT%\tests\jump_arms_selftest.cpp" "%TRANSITION_ROOT%\src\InlineHook.cpp" "%TRANSITION_ROOT%\src\Log.cpp" /Fe"%TRANSITION_ROOT%\build\jump-arms-test\jump_arms_test.exe" /Fo"%TRANSITION_ROOT%\build\jump-arms-test\\" /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
"%TRANSITION_ROOT%\build\jump-arms-test\jump_arms_test.exe"

