@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cmake -S . -B build_asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLRC_ENABLE_QT=OFF -DCMAKE_CXX_FLAGS="/fsanitize=address /Zi /W4 /wd4100 /wd4244 /wd4996" -DCMAKE_EXE_LINKER_FLAGS="/fsanitize=address"
echo CONFIG_EXIT=%ERRORLEVEL%
