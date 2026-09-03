@echo off
setlocal EnableDelayedExpansion

echo.
echo +===============================================================+
echo          Remote Controller - Build Script
echo +===============================================================+
echo.

:: Check for compiler
where cl >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Microsoft Visual C++ compiler not found!
    echo Please run this script from Visual Studio Developer Command Prompt
    echo or install Visual Studio Build Tools.
    echo.
    echo You can also use MinGW-w64:
    echo   g++ -o rc_agent.exe rc_agent.cpp -lws2_32 -ladvapi32 -liphlpapi -lcrypt32 -lgdi32 -luser32 -static
    echo   g++ -o rc_viewer.exe rc_viewer.cpp -lws2_32 -ladvapi32 -liphlpapi -lcrypt32 -lgdi32 -luser32 -static
    echo   g++ -o rc_service.exe rc_service.cpp -lws2_32 -ladvapi32 -liphlpapi -lcrypt32 -lshell32 -luser32 -static
    pause
    exit /b 1
)

echo [INFO] Compiler found. Starting build...
echo.

:: Common compiler flags
set CFLAGS=/nologo /EHsc /O2 /W3 /std:c++17 /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN
set LIBS=Ws2_32.lib Iphlpapi.lib Crypt32.lib Advapi32.lib User32.lib Gdi32.lib Shell32.lib Winmm.lib d3d11.lib d3dcompiler.lib Ole32.lib
set IMGUI_SRC=imgui.cpp imgui_draw.cpp imgui_widgets.cpp imgui_tables.cpp imgui_impl_win32.cpp imgui_impl_dx11.cpp

:: Build Agent
echo [BUILD] Compiling rc_agent.exe (GUI Subsystem)...
cl %CFLAGS% rc_agent.cpp /Fe:rc_agent.exe /link %LIBS% /SUBSYSTEM:WINDOWS
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build rc_agent.exe
    goto :error
)
echo [OK] rc_agent.exe built successfully!
echo.

:: Build Viewer
echo [BUILD] Compiling rc_viewer.exe (GUI Subsystem)...
cl %CFLAGS% rc_viewer.cpp /Fe:rc_viewer.exe /link %LIBS% /SUBSYSTEM:WINDOWS
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build rc_viewer.exe
    goto :error
)
echo [OK] rc_viewer.exe built successfully!
echo.

:: Build Service
echo [BUILD] Compiling rc_service.exe...
cl %CFLAGS% rc_service.cpp /Fe:rc_service.exe /link %LIBS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build rc_service.exe
    goto :error
)
echo [OK] rc_service.exe built successfully!
echo.

:: Cleanup object files
echo [CLEANUP] Removing temporary files...
del /q *.obj 2>nul

echo.
echo +===============================================================+
echo                     BUILD SUCCESSFUL!
echo +===============================================================+
echo   Created files:
echo     - rc_agent.exe   : Run on remote PC to allow control
echo     - rc_viewer.exe  : Run to connect and view remote PC
echo     - rc_service.exe : Background service for unattended access
echo +===============================================================+
echo.
pause
exit /b 0

:error
echo.
echo Build failed! Check errors above.
pause
exit /b 1

