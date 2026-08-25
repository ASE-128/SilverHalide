@echo off
setlocal
set "ROOT=%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL & exit /b 1)
cd /d "%ROOT%"
cmake --build build_gui -j 4 > "C:\Users\tmlde\.claude\jobs\12178483\tmp\rebuild_build4.log" 2>&1
echo BUILD4_EXIT=%errorlevel%
