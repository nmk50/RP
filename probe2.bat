@echo off
call "D:\vc\VC\Auxiliary\Build\vcvarsall.bat" x64 >/dev/null
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /MD /O2 /DNDEBUG /I"libs\raylib-win64\include" /Fo"out\obj\\" src\main.cpp /link /SUBSYSTEM:CONSOLE libs\raylib-win64\lib\raylib.lib winmm.lib opengl32.lib gdi32.lib user32.lib shell32.lib /OUT:"out\rp.exe"
echo EXITCODE=%ERRORLEVEL%
