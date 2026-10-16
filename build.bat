@echo off
rem Build OpenSubst with MSVC (Visual Studio 2019/2022 Build Tools).
rem   build.bat          release build, x64    -> bin\x64\OpenSubst.exe
rem   build.bat x86      release build, 32-bit -> bin\x86\OpenSubst.exe
rem   build.bat arm64    release build, ARM64 (needs the ARM64 build tools)
setlocal
cd /d "%~dp0"

set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
set VCARCH=%ARCH%
if /i "%ARCH%"=="arm64" set VCARCH=x64_arm64

where cl >nul 2>nul
if not errorlevel 1 goto havecl
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%i"
if not defined VSDIR if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools" set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>nul
:havecl

set OUT=bin\%ARCH%
set OBJ=obj\%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

rc /nologo /fo "%OBJ%\OpenSubst.res" res\OpenSubst.rc
if errorlevel 1 exit /b 1

cl /nologo /c /TC /std:c17 /utf-8 /O2 /MT /W4 /WX /permissive- /guard:cf /sdl /GS ^
   /DNDEBUG /Fo"%OBJ%\\" src\main.c src\ui.c src\engine.c src\drives.c src\uac.c ^
   src\recycle.c src\settings.c src\lang.c src\theme.c src\gfx.c src\links.c
if errorlevel 1 exit /b 1

rem Hardening: ASLR with high entropy, DEP, CFG, CET shadow stack (x64).
rem DLLs that are not KnownDLLs are delay-loaded: they are then loaded only
rem from System32 (SetDefaultDllDirectories in main.c), never from the exe's
rem folder, and the elevated helper mode does not load the UI ones at all.
set LINKARCH=
if /i "%ARCH%"=="x64" set LINKARCH=/HIGHENTROPYVA /CETCOMPAT
if /i "%ARCH%"=="arm64" set LINKARCH=/HIGHENTROPYVA

link /nologo /OUT:"%OUT%\OpenSubst.exe" /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /GUARD:CF %LINKARCH% ^
   "%OBJ%\*.obj" "%OBJ%\OpenSubst.res" ^
   kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib ^
   uuid.lib comctl32.lib comdlg32.lib uxtheme.lib mpr.lib gdiplus.lib delayimp.lib ^
   /DELAYLOAD:gdiplus.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:comdlg32.dll ^
   /DELAYLOAD:uxtheme.dll /DELAYLOAD:mpr.dll
if errorlevel 1 exit /b 1

echo Built %OUT%\OpenSubst.exe
exit /b 0

:novs
echo Visual Studio C++ build tools not found.
exit /b 1
