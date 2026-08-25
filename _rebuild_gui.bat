@echo off
setlocal
set "ROOT=%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo VCVARS_FAIL & exit /b 1)
cd /d "%ROOT%"
del /q build_gui\.ninja_log build_gui\.ninja_deps 2>nul
cmake -S "%ROOT%lightroom-clone-cpp" -B "%ROOT%build_gui" -G Ninja -DCMAKE_PREFIX_PATH="%ROOT%qt\6.8.3\msvc2022_64" -DCMAKE_BUILD_TYPE=Release > "C:\Users\tmlde\.claude\jobs\12178483\tmp\rebuild_cfg.log" 2>&1
if errorlevel 1 (echo CFG_FAIL & exit /b 1)
cmake --build build_gui -j 8 > "C:\Users\tmlde\.claude\jobs\12178483\tmp\rebuild_build.log" 2>&1
echo REBUILD_EXIT=%errorlevel%
