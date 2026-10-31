@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist build\stereo-color-tests mkdir build\stereo-color-tests
cl /nologo /std:c++17 /O2 /EHsc /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc tools\stereo_color_gl_tests.cpp src\StereoRenderer.cpp src\GL.cpp /Febuild\stereo-color-tests\stereo_color_gl_tests.exe /Fobuild\stereo-color-tests\ /link user32.lib gdi32.lib opengl32.lib
if errorlevel 1 exit /b 1
build\stereo-color-tests\stereo_color_gl_tests.exe
