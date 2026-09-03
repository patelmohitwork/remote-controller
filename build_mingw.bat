@echo off
echo.
echo +===============================================================+
echo        Remote Controller - MinGW Build Script
echo +===============================================================+
echo.

:: Check for g++
where g++ >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo [ERROR] MinGW g++ compiler not found!
    echo Please install MinGW-w64 and add it to PATH.
    echo Download: https://www.mingw-w64.org/
    pause
    exit /b 1
)

echo [INFO] MinGW compiler found. Starting build...
echo.

set CFLAGS=-std=c++17 -O2 -Wall -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601
set LIBS=-lws2_32 -ladvapi32 -liphlpapi -lcrypt32 -lgdi32 -luser32 -lshell32 -lole32 -loleaut32 -luuid -lwinmm -static

:: Build Agent
echo [BUILD] Compiling rc_agent.exe (GUI Subsystem)...
g++ %CFLAGS% -mwindows -o rc_agent.exe rc_agent.cpp %LIBS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build rc_agent.exe
    goto :error
)
echo [OK] rc_agent.exe built successfully!
echo.

:: Build Viewer  
echo [BUILD] Compiling rc_viewer.exe (GUI Subsystem)...
g++ %CFLAGS% -mwindows -o rc_viewer.exe rc_viewer.cpp %LIBS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build rc_viewer.exe
    goto :error
)
echo [OK] rc_viewer.exe built successfully!
echo.

:: Build Service
echo [BUILD] Compiling rc_service.exe...
g++ %CFLAGS% -o rc_service.exe rc_service.cpp %LIBS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build rc_service.exe
    goto :error
)
echo [OK] rc_service.exe built successfully!
echo.

echo.
echo +===============================================================+
echo                     BUILD SUCCESSFUL!
echo +===============================================================+
echo.
pause
exit /b 0

:error
echo Build failed! Check errors above.
pause
exit /b 1

