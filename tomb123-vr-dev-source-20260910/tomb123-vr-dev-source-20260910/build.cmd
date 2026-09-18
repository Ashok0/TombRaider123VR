@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "ProjectRoot=%~dp0"
if not exist "%ProjectRoot%build" mkdir "%ProjectRoot%build"
cd /d "%ProjectRoot%build"
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT ..\src\inject.cpp /Fe:inject.exe /link user32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /I..\deps\openxr\include /LD ..\src\prototype.cpp ..\src\xr_bridge.cpp ..\src\vr_menu.cpp ..\src\firstperson_mesh.cpp ..\src\native_eye.cpp /Fe:tombvr.dll /link user32.lib opengl32.lib gdi32.lib ..\deps\openxr\lib\openxr_loader.lib delayimp.lib /DELAYLOAD:openxr_loader.dll
