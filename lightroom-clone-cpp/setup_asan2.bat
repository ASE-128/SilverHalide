@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cl /fsanitize=address /EHsc /c build_asan_test.cpp
echo CL_EXIT=%ERRORLEVEL%
