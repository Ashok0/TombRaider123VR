@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\build"
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT ..\research\firstperson_mesh_tests.cpp /Fe:firstperson_mesh_tests.exe /link user32.lib gdi32.lib opengl32.lib
if errorlevel 1 exit /b 1
"firstperson_mesh_tests.exe"
