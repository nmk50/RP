@echo off
REM Headless terrain screenshot harness (out/mapshot.exe) - not part of the game build
setlocal
set VCvars=D:\vc\VC\Auxiliary\Build\vcvarsall.bat
set RAYDIR=%~dp0libs\raylib-win64
set XMLDIR=%~dp0libs\tinyxml2
set OBJDIR=%~dp0out\obj_mapshot
if not exist "%OBJDIR%" mkdir "%OBJDIR%"
call "%VCvars%" x64 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /MD /O2 /DNDEBUG /I"%RAYDIR%\include" /I"%XMLDIR%" /I"%~dp0src" ^
   /Fo"%OBJDIR%\\" ^
   src\Game.cpp src\Templates.cpp src\TileMap.cpp src\Pathfinding.cpp src\Renderer.cpp src\Debug.cpp out\mapshot.cpp "%XMLDIR%\tinyxml2.cpp" ^
   /link /SUBSYSTEM:CONSOLE "%RAYDIR%\lib\raylib.lib" winmm.lib opengl32.lib gdi32.lib user32.lib shell32.lib ^
   /OUT:"%~dp0out\mapshot.exe"
if errorlevel 1 ( echo MAPSHOT BUILD FAILED & exit /b 1 )
echo MAPSHOT BUILD OK