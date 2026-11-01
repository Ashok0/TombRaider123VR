@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "QUEUE_ROOT=%~dp0.."
if not exist "%QUEUE_ROOT%\build\ground-queue-test" mkdir "%QUEUE_ROOT%\build\ground-queue-test"
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%QUEUE_ROOT%\src" /I"%QUEUE_ROOT%\third_party\openvr\headers" "%QUEUE_ROOT%\tests\ground_queue_test.cpp" "%QUEUE_ROOT%\src\InlineHook.cpp" "%QUEUE_ROOT%\src\Log.cpp" /Fe"%QUEUE_ROOT%\build\ground-queue-test\ground_queue_test.exe" /Fo"%QUEUE_ROOT%\build\ground-queue-test\\" /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
"%QUEUE_ROOT%\build\ground-queue-test\ground_queue_test.exe"
