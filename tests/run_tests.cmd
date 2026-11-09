@echo off
rem Builds and runs the tests with Visual Studio 2019. The game isn't needed.
setlocal
if not defined ProgramFiles(x86) set "ProgramFiles(x86)=C:\Program Files (x86)"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -version [16.0^,17.0^) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio 2019 with the C++ tools not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
cl /nologo /EHsc /std:c++17 /O2 /Fobin\ /Febin\tests.exe main.cpp test_hands.cpp test_wrist_hud.cpp test_pose.cpp %* || exit /b 1
bin\tests.exe
