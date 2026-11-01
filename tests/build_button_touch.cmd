@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "BUTTON_ROOT=%~dp0.."
if not exist "%BUTTON_ROOT%\build\button-touch-test" mkdir "%BUTTON_ROOT%\build\button-touch-test"
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%BUTTON_ROOT%\src" /I"%BUTTON_ROOT%\third_party\openvr\headers" "%BUTTON_ROOT%\tests\button_touch_selftest.cpp" "%BUTTON_ROOT%\src\InlineHook.cpp" "%BUTTON_ROOT%\src\Log.cpp" /Fe"%BUTTON_ROOT%\build\button-touch-test\button_touch_test.exe" /Fo"%BUTTON_ROOT%\build\button-touch-test\\" /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
"%BUTTON_ROOT%\build\button-touch-test\button_touch_test.exe"
