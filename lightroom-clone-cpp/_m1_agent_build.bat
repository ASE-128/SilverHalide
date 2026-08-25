@echo off
setlocal
cd /d "%~dp0.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed
  exit /b 1
)
echo === CMake configure ===
cmake -S lightroom-clone-cpp -B build_headless -G Ninja -DCMAKE_BUILD_TYPE=Release -DLRC_ENABLE_QT=OFF
if errorlevel 1 (
  echo [ERROR] CMake configure failed
  exit /b 1
)
echo === CMake build ===
cmake --build build_headless -j
if errorlevel 1 (
  echo [ERROR] CMake build failed
  exit /b 1
)
echo === BUILD OK ===
