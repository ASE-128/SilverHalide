@echo off
setlocal
cd /d "%~dp0"
build_headless\tests\lrc_tests.exe
echo CTEST_EXIT=%errorlevel%
