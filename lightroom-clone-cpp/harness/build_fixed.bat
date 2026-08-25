@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\lrcproj2\harness
cl /nologo /O2 /EHsc /std:c++20 /MT /Zi /I C:\lrcproj2\src ^
   main.cpp Parallel.cpp ^
   /link /out:harness_fixed.exe /DEBUG dbghelp.lib
echo BUILD_EXIT=%ERRORLEVEL%
