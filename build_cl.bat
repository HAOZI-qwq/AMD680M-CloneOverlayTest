@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if errorlevel 1 (
  echo [ERROR] cl.exe not found. Open "x64 Native Tools Command Prompt for VS 2022" first.
  pause
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /W4 /O2 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   src\main.cpp /Fe:AMDCloneOverlayProbe.exe ^
   /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib dcomp.lib user32.lib gdi32.lib ole32.lib

if errorlevel 1 (
  echo [ERROR] Build failed.
  pause
  exit /b 1
)

echo Build OK: %CD%\AMDCloneOverlayProbe.exe
pause
