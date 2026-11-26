@echo off
rem Builds app\build\tests.exe from tests.cpp (the whole program plus its tests) and runs it.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\tests mkdir build\tests

where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=%SystemDrive%\Program Files (x86)"
"%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > build\vs.txt
set /p VS=<build\vs.txt
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
:have_cl

set FLAGS=/nologo /FC /std:c++20 /utf-8 /W4 /wd4100 /wd4505 /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /Zi /FS /Od /MT
set LIBS=user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib d3d11.lib dxgi.lib dwmapi.lib dbghelp.lib imm32.lib mmdevapi.lib mfplat.lib mfuuid.lib windowscodecs.lib oleaut32.lib winhttp.lib bcrypt.lib shcore.lib

cl %FLAGS% tests.cpp /Fobuild\tests\ /Fdbuild\tests\ /Febuild\tests.exe /link /SUBSYSTEM:CONSOLE /INCREMENTAL:NO %LIBS% || exit /b 1
build\tests.exe
