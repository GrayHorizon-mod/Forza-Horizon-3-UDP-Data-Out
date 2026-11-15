@echo off
rem Build FH3DataOut.exe (x64, release, static CRT) with VS Build Tools.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo VS Build Tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
rc /nologo fh3dataout.rc || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 fh3dataout.cpp fh3dataout.res /Fe:FH3DataOut.exe /link /SUBSYSTEM:CONSOLE || exit /b 1
del fh3dataout.obj 2>nul
echo BUILD OK
