@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "QUEST_ROOT=%~dp0.."
set "QUEST_TEST_OUT=%~1"
if "%QUEST_TEST_OUT%"=="" set "QUEST_TEST_OUT=%QUEST_ROOT%\build\quest-camera-test"
if not exist "%QUEST_TEST_OUT%" mkdir "%QUEST_TEST_OUT%"
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"%QUEST_ROOT%\src" /I"%QUEST_ROOT%\third_party\openvr\headers" "%QUEST_ROOT%\tests\quest_camera_test.cpp" "%QUEST_ROOT%\src\InlineHook.cpp" "%QUEST_ROOT%\src\Log.cpp" /Fe"%QUEST_TEST_OUT%\quest_camera_test.exe" /Fo"%QUEST_TEST_OUT%\\" /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
"%QUEST_TEST_OUT%\quest_camera_test.exe"
