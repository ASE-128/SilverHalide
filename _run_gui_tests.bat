@echo off
setlocal
set "ROOT=%~dp0"
set "QT_BIN=%ROOT%qt\6.8.3\msvc2022_64\bin"
set "PATH=%QT_BIN%;%PATH%"
set "QT_PLUGIN_PATH=%ROOT%build_gui\platforms"
cd /d "%ROOT%"
build_gui\tests\lrc_tests.exe
echo GUI_TESTS_EXIT=%errorlevel%
