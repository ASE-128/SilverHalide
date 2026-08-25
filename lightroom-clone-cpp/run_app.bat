@echo off
:: Launch Silver Halide.
:: The deployed exe (with Qt runtime + platform plugin next to it via
:: windeployqt) lives at build_gui/Release/SilverHalide.exe (VS generator).
if not exist "build_gui\Release\SilverHalide.exe" (
  echo [ERROR] SilverHalide.exe not found. Rebuild first:
  echo   cmake --build build_gui --config Release --parallel
  pause
  exit /b 1
)
echo Starting Silver Halide...
start "" "build_gui\Release\SilverHalide.exe"
