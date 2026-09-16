@echo off
setlocal
cd /d "%~dp0"

echo === AMD Clone Overlay Probe build ===

where cmake >nul 2>nul
if errorlevel 1 (
  echo [ERROR] CMake not found.
  echo Install Visual Studio 2022 with "Desktop development with C++" and CMake tools.
  pause
  exit /b 1
)

cmake -S . -B build -A x64
if errorlevel 1 goto :fail
cmake --build build --config Release
if errorlevel 1 goto :fail

copy /Y "build\Release\AMDCloneOverlayProbe.exe" ".\AMDCloneOverlayProbe.exe" >nul

echo.
echo Build OK: %CD%\AMDCloneOverlayProbe.exe
pause
exit /b 0

:fail
echo.
echo [ERROR] Build failed. Run this from a Visual Studio Developer Command Prompt,
echo or install Visual Studio 2022 "Desktop development with C++".
pause
exit /b 1
