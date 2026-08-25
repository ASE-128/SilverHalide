@echo off
setlocal
set "ROOT=%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL & exit /b 1)
set "QTDIR=%ROOT%qt\6.8.3\msvc2022_64"
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --compiler-runtime "%ROOT%build_gui\SilverHalide.exe"
echo DEPLOY_EXIT=%errorlevel%
