@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "PATH=%~dp0qt\6.8.3\msvc2022_64\bin;%PATH%"
"%~dp0build_gui\tests\lrc_qt_tests.exe" -o "%~dp0build_gui\qt_tests_out.txt,txt" 2>&1
exit /b %errorlevel%
