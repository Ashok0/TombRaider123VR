@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0..\build"
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT ..\research\classic_math_tests.cpp /Fe:classic_math_tests.exe
if errorlevel 1 exit /b 1
"classic_math_tests.exe"
