@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL & exit /b 1)
if "%1"=="gui" (
  cmake --build build_gui -j 4
  echo GUI_EXIT=%errorlevel%
) else (
  cmake --build build_headless -j 4
  echo HEADLESS_EXIT=%errorlevel%
)
