@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "CROUCH_ROOT=%~dp0.."
if not exist "%CROUCH_ROOT%\build\crouch-transition-test" mkdir "%CROUCH_ROOT%\build\crouch-transition-test"
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%CROUCH_ROOT%\src" /I"%CROUCH_ROOT%\third_party\openvr\headers" "%CROUCH_ROOT%\tests\crouch_transition_test.cpp" "%CROUCH_ROOT%\src\InlineHook.cpp" "%CROUCH_ROOT%\src\Log.cpp" /Fe"%CROUCH_ROOT%\build\crouch-transition-test\crouch_transition_test.exe" /Fo"%CROUCH_ROOT%\build\crouch-transition-test\\" /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
"%CROUCH_ROOT%\build\crouch-transition-test\crouch_transition_test.exe"
