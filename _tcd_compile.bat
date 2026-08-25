@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /TP /std:c++20 -DLIBRAW_NODLL=1 -DNOMINMAX -DSQLITE_THREADSAFE=1 -DSQLITE_USE_URI=1 -I build_gui\tests\lrc_tests_autogen\include -I lightroom-clone-cpp\src -I lightroom-clone-cpp\third_party\libraw -I lightroom-clone-cpp\third_party\lcms2\include -I lightroom-clone-cpp\third_party\sqlite -I lightroom-clone-cpp\third_party\stb -I lightroom-clone-cpp\third_party\pugixml -I lightroom-clone-cpp\third_party\Catch2\src\catch2\.. -I build_gui\third_party\Catch2\generated-includes /MD /EHsc /c lightroom-clone-cpp\tests\TestProfileIO.cpp /Fo:tmp_tpio.obj
