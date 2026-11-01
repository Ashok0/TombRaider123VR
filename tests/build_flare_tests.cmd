@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0.."
if not exist build\physical-flare-test mkdir build\physical-flare-test
cl /nologo /std:c++17 /Od /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DFLARE_TR123 /Isrc tests\flare_throw_test.cpp /Febuild\physical-flare-test\flare123.exe /Fobuild\physical-flare-test\flare123.obj
if errorlevel 1 exit /b 1
build\physical-flare-test\flare123.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc /Ithird_party\openvr\headers tests\flare_input_test.cpp src\InlineHook.cpp src\Log.cpp /Febuild\physical-flare-test\flare_input123.exe /Fobuild\physical-flare-test\ /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
build\physical-flare-test\flare_input123.exe
exit /b %errorlevel%
