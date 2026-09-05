@echo off
echo Building 64-bit (x64) version...
echo.
echo IMPORTANT: You need the FTDI libraries in ftdilib\amd64\
echo.

REM Check if cl.exe is already available (e.g., running from a dev prompt)
where cl >nul 2>nul
if %errorlevel%==0 goto :build

REM Try vswhere first (most reliable method)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do (
        if exist "%%i\Common7\Tools\VsDevCmd.bat" (
            echo Found Visual Studio at %%i
            call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64
            goto :build
        )
    )
)

REM Fallback: search both Program Files locations
for %%p in ("%ProgramFiles%" "%ProgramFiles(x86)%") do (
    for /f "delims=" %%i in ('where /r %%p VsDevCmd.bat 2^>nul') do (
        echo Found Visual Studio at %%i
        call "%%i" -arch=x64
        goto :build
    )
)

echo ERROR: Could not find Visual Studio x64 tools
echo Please run from "x64 Native Tools Command Prompt for VS"
pause
exit /b 1

:build
if not exist "ftdilib\amd64\ftd2xx.lib" (
    echo ERROR: ftdilib\amd64\ftd2xx.lib not found!
    echo Please set up the ftdilib folder first.
    pause
    exit /b 1
)
nmake clean
nmake
pause