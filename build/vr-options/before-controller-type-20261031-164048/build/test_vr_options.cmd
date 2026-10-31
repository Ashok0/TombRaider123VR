@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build\vr-menu-test mkdir build\vr-menu-test
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc /Ithird_party\openvr\headers tools\vr_menu_tests.cpp src\Log.cpp /Febuild\vr-menu-test\vr_menu_tests.exe /Fobuild\vr-menu-test\ /link /OPT:REF user32.lib gdi32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /Od /Gy /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc /Ithird_party\openvr\headers tools\vr_native_menu_tests.cpp src\InlineHook.cpp src\Log.cpp /Febuild\vr-menu-test\vr_native_menu_tests.exe /Fobuild\vr-menu-test\ /link /OPT:REF user32.lib
if errorlevel 1 exit /b 1
build\vr-menu-test\vr_native_menu_tests.exe
if errorlevel 1 exit /b 1
pushd build\vr-menu-test
vr_menu_tests.exe
set RESULT=%ERRORLEVEL%
popd
exit /b %RESULT%
