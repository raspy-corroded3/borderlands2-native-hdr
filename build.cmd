@echo off
rem Build bl2hdr (32-bit d3d9.dll). Usage: build.cmd [Release|Debug]
rem Uses VS Build Tools 2026 (x86 toolchain), CMake + Ninja bundled with it.
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio Build Tools not found & exit /b 1)
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 -no_logo || exit /b 1
cd /d "%~dp0"
cmake -S . -B build\%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build\%CONFIG% || exit /b 1
echo.
echo Built: %~dp0build\%CONFIG%\d3d9.dll
