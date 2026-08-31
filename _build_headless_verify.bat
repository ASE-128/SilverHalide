@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed
  exit /b 1
)
cmake --build "%ROOT%build_headless" -j
if errorlevel 1 (
  echo [ERROR] Build failed
  exit /b 1
)
ctest --test-dir "%ROOT%build_headless" --output-on-failure
exit /b %errorlevel%
