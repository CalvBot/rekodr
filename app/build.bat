@echo off
rem Builds app\build\rekodr.exe from the single translation unit rekodr.cpp. `build debug` for a debug build,
rem `build release` for an optimised build plus dist\rekodr-setup.exe and its update manifest (release.ps1).
setlocal
cd /d "%~dp0"
if not exist build mkdir build

rem Find Visual Studio's compiler unless it's already set up.
where cl >nul 2>nul
if not errorlevel 1 goto have_cl
rem Shells like Git Bash drop variables with brackets in their names, such as ProgramFiles(x86).
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=%SystemDrive%\Program Files (x86)"
"%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > build\vs.txt
set /p VS=<build\vs.txt
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
:have_cl

rem /pathmap writes source paths as .\... and /PDBALTPATH names the .pdb without its folder, so the exe holds no
rem path from this PC. /pathmap needs /experimental:deterministic, which warns (5048) on __DATE__ and __TIME__.
set FLAGS=/nologo /std:c++20 /utf-8 /W4 /wd4100 /wd4505 /wd5048 /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /Zi /FS /experimental:deterministic /pathmap:"%CD%"=.
if "%1"=="debug" (set FLAGS=%FLAGS% /Od /MTd /DDEBUG) else (set FLAGS=%FLAGS% /O2 /MT)
rem The service's signing key: REKODR_SERVICE_KEY, or the file ~\.secrets\rekodr-service-key. It is never in the repo.
if "%REKODR_SERVICE_KEY%"=="" if exist "%USERPROFILE%\.secrets\rekodr-service-key" set /p REKODR_SERVICE_KEY=<"%USERPROFILE%\.secrets\rekodr-service-key"
if "%REKODR_SERVICE_KEY%"=="" (echo error: no service key: set REKODR_SERVICE_KEY or write ~\.secrets\rekodr-service-key& exit /b 1)
set FLAGS=%FLAGS% /DREKODR_SERVICE_KEY=\"%REKODR_SERVICE_KEY%\"
set LIBS=user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib d3d11.lib dxgi.lib dwmapi.lib dbghelp.lib imm32.lib mmdevapi.lib mfplat.lib mfuuid.lib windowscodecs.lib oleaut32.lib winhttp.lib bcrypt.lib shcore.lib

rem ASCII only: no byte above 127 in the sources, and no non-ASCII text written as hex escapes in strings either
rem (a middle dot as C2 B7, say). Icon font glyphs (private use area, escapes starting EE or EF) are pictures, not
rem text, and are allowed.
powershell -NoProfile -Command "$bad = 0; Get-ChildItem *.cpp,*.h,*.rc,*.bat | ForEach-Object { $n = 0; foreach ($l in [IO.File]::ReadAllLines($_.FullName, [Text.Encoding]::GetEncoding(28591))) { $n++; if ($l -match '[^\x00-\x7F]' -or $l -match '\\x([cd][0-9a-f]|e[0-9a-d]|f[0-4])') { Write-Host ('{0}({1}): error: non-ASCII text: {2}' -f $_.Name, $n, $l.Trim()); $bad++ } } }; exit [int]($bad -gt 0)" || exit /b 1

rc /nologo /fo build\rekodr.res rekodr.rc || exit /b 1
cl %FLAGS% rekodr.cpp build\rekodr.res /Fobuild\ /Fdbuild\ /Febuild\rekodr.exe /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /PDBALTPATH:%%_PDB%% %LIBS% || exit /b 1
if "%1"=="release" pwsh -NoProfile -ExecutionPolicy Bypass -File release.ps1 || exit /b 1
