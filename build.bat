@echo off
REM ==================================================================
REM  RP build script - C++ remake of RWX (Rusted Warfare Extension)
REM  Uses MSVC toolchain via vcvarsall + cl.exe/link.exe (x64)
REM ==================================================================
setlocal

set VCvars=D:\vc\VC\Auxiliary\Build\vcvarsall.bat
set RAYDIR=%~dp0libs\raylib-win64
set STBDIR=%~dp0libs\stb
set XMLDIR=%~dp0libs\tinyxml2
set OUTDIR=%~dp0out
set OBJDIR=%OUTDIR%\obj

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OBJDIR%" mkdir "%OBJDIR%"

echo [1/3] Initialising MSVC x64 environment...
call "%VCvars%" x64 >nul
if errorlevel 1 ( echo vcvarsall failed & exit /b 1 )

echo [2/3] Compiling sources...
cd /d "%~dp0"

REM Locate tinyxml2 sources
set XMLSRC=
if exist "%XMLDIR%\tinyxml2.cpp" set XMLSRC=%XMLDIR%\tinyxml2.cpp

cl /nologo /std:c++17 /EHsc /MD /O2 /DNDEBUG ^
   /I"%RAYDIR%\include" /I"%STBDIR%" /I"%XMLDIR%" ^
   /Fo"%OBJDIR%\\" ^
   src\*.cpp "%XMLSRC%" ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup ^
   "%RAYDIR%\lib\raylib.lib" winmm.lib opengl32.lib gdi32.lib user32.lib shell32.lib ^
   /OUT:"%OUTDIR%\rp.exe"

if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )

echo [3/3] Copying runtime dll next to exe...
copy /y "%RAYDIR%\lib\raylib.dll" "%OUTDIR%\raylib.dll" >nul

echo BUILD OK: %OUTDIR%\rp.exe
endlocal
