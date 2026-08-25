# ============================================================================
# setup_environment.ps1 - One-time build environment setup for Silver Halide
# Target OS: Windows 11 (x64). Run in an ELEVATED PowerShell:
#   Right-click PowerShell -> "Run as administrator", then:
#     powershell -ExecutionPolicy Bypass -File setup_environment.ps1
# Or open Terminal as admin and run:  ./setup_environment.ps1
#
# Installs (all via winget, built into Windows 11):
#   [1] Visual Studio 2022 Community + "Desktop development with C++" workload
#   [2] CMake (>= 3.25) + Ninja
#   [3] Python 3.12 (optional: icon / BMP generation scripts)
#   [4] Qt 6.8.3 for MSVC 2022 x64 (via aqtinstall)  ->  ./qt/6.8.3/msvc2022_64
#   [5] NSIS 3 (optional: installer packaging)
#
# Expected time: [1] is the long one (10-40 min, may need a reboot).
# Everything else is fast. Steps [3] and [5] can be skipped if not needed.
# ============================================================================

$ErrorActionPreference = "Continue"

Write-Host "== [1/5] Visual Studio 2022 Community + C++ desktop workload =="
winget install --id Microsoft.VisualStudio.2022.Community --silent `
  --accept-source-agreements --accept-package-agreements `
  --override "--wait --quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"

Write-Host ""
Write-Host "== [2/5] CMake (>= 3.25) and Ninja =="
winget install --id Kitware.CMake --silent --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja --silent --accept-source-agreements --accept-package-agreements

Write-Host ""
Write-Host "== [3/5] Python 3.12 (optional) =="
winget install --id Python.Python.3.12 --silent --accept-source-agreements --accept-package-agreements

Write-Host ""
Write-Host "== [4/5] Qt 6.8.3 for MSVC 2022 x64 (via aqtinstall) =="
python -m pip install --upgrade aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O qt

Write-Host ""
Write-Host "== [5/5] NSIS 3 (optional: installer packaging) =="
winget install --id NSIS.NSIS --silent --accept-source-agreements --accept-package-agreements

Write-Host ""
Write-Host "=========================================================="
Write-Host "Environment setup finished."
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. If a reboot was requested by the VS installer, reboot first."
Write-Host "  2. Verify Qt landed at  .\qt\6.8.3\msvc2022_64\bin\qmake.exe"
Write-Host "  3. Open 'x64 Native Tools Command Prompt for VS 2022' (start menu),"
Write-Host "     cd to this folder, then run:"
Write-Host "        build_x64.bat      (full GUI build  -> build_gui\SilverHalide.exe)"
Write-Host "        _run_gui_tests.bat (GUI test suite  -> GUI_TESTS_EXIT=0)"
Write-Host "     See docs/MIGRATION.md section 4 for the full first-build checklist."
Write-Host "=========================================================="
