@echo off
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo [ERROR] vcvars64.bat failed
  exit /b 1
)

set "QT_PREFIX=%ROOT%qt\6.8.3\msvc2022_64"

echo === Configuring x64 ===
cmake -S "%ROOT%lightroom-clone-cpp" -B "%ROOT%build_gui" -G Ninja -DCMAKE_PREFIX_PATH="%QT_PREFIX%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
  echo [ERROR] CMake configure failed
  exit /b 1
)

echo === Building ===
cmake --build "%ROOT%build_gui" -j
if errorlevel 1 (
  echo [ERROR] CMake build failed
  exit /b 1
)

echo === Build OK ===
dir "%ROOT%build_gui\SilverHalide.exe"
