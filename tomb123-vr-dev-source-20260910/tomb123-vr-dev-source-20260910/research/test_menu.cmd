@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\build"
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT ..\research\menu_upload_tests.cpp ..\src\vr_menu.cpp /Fe:menu_upload_tests.exe /link user32.lib gdi32.lib opengl32.lib
if errorlevel 1 exit /b 1
menu_upload_tests.exe
