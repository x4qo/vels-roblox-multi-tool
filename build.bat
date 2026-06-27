@echo off
setlocal
set GPP=g++

where %GPP% >nul 2>nul
if errorlevel 1 (
    echo g++ not found on PATH. Install MinGW-w64 first ^(e.g. winget install BrechtSanders.WinLibs.POSIX.UCRT^) and restart your shell.
    pause
    exit /b 1
)

where windres >nul 2>nul
if errorlevel 1 (
    echo windres not found on PATH ^(it ships with MinGW-w64^). Install MinGW-w64 first and restart your shell.
    pause
    exit /b 1
)

windres src\app.rc -O coff -o src\app_icon.res
if errorlevel 1 (
    echo Failed to compile app icon resource.
    pause
    exit /b 1
)

echo Compiling - this takes 20-60+ seconds with no output, that's normal, g++ doesn't print progress...

%GPP% -O2 -std=c++17 -municode -mwindows ^
  -I src\imgui -I src\imgui\backends ^
  src\main.cpp src\backend.cpp src\login.cpp ^
  src\imgui\imgui.cpp src\imgui\imgui_draw.cpp src\imgui\imgui_tables.cpp src\imgui\imgui_widgets.cpp ^
  src\imgui\backends\imgui_impl_win32.cpp src\imgui\backends\imgui_impl_dx11.cpp ^
  src\app_icon.res ^
  -o VelsMultiTool.exe ^
  -ld3d11 -ldxgi -ld3dcompiler -lshell32 -liphlpapi -luser32 -lgdi32 -ldwmapi -lwinhttp -lws2_32 -lcrypt32 -lole32 -lwindowscodecs ^
  -static-libgcc -static-libstdc++ -static

if errorlevel 1 (
    echo Build failed.
    pause
    exit /b 1
)
echo Build succeeded: VelsMultiTool.exe
pause
