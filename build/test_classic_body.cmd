@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist build\classic-turn mkdir build\classic-turn
cl /nologo /std:c++17 /EHsc /O2 tools\classic_body_tests.cpp /Febuild\classic-turn\classic_body_tests.exe /Fobuild\classic-turn\classic_body_tests.obj
if errorlevel 1 exit /b 1
build\classic-turn\classic_body_tests.exe
