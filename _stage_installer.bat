@echo off
setlocal
set "ROOT=%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL & exit /b 1)

set "QTDIR=%ROOT%qt\6.8.3\msvc2022_64"
set "ORT_ROOT=%ROOT%onnxruntime\onnxruntime-win-x64-1.23.2"
set "STAGE=%ROOT%build_gui\install_stage"

if not exist "%STAGE%" mkdir "%STAGE%"
if not exist "%STAGE%\models" mkdir "%STAGE%\models"

echo === Copying SilverHalide.exe to stage ===
copy /y "%ROOT%build_gui\SilverHalide.exe" "%STAGE%\SilverHalide.exe" >nul
if errorlevel 1 (echo COPY_EXE_FAIL & exit /b 1)

echo === Running windeployqt (release, no-translations, compiler-runtime) ===
"%QTDIR%\bin\windeployqt.exe" --release --no-translations --compiler-runtime "%STAGE%\SilverHalide.exe"
if errorlevel 1 (echo WINDEPLOYQT_FAIL & exit /b 1)

echo === Copying ONNX Runtime DLLs to stage ===
copy /y "%ORT_ROOT%\lib\onnxruntime.dll" "%STAGE%\onnxruntime.dll" >nul
if errorlevel 1 (echo COPY_ORT_DLL_FAIL & exit /b 1)
copy /y "%ORT_ROOT%\lib\onnxruntime_providers_shared.dll" "%STAGE%\onnxruntime_providers_shared.dll" >nul
if errorlevel 1 (echo COPY_ORT_SHARED_FAIL & exit /b 1)

echo === Stage contents ===
dir /b "%STAGE%"
echo === Stage\models contents ===
dir /b "%STAGE%\models"

echo STAGE_DONE
