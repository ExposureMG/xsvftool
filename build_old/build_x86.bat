REM ===== build-x86.bat =====
@echo off
echo Building 32-bit (x86) version...
echo.
echo IMPORTANT: You need the FTDI libraries in ftdilib\i386\
echo.

REM Check if already in a VS command prompt
if "%VSCMD_ARG_TGT_ARCH%"=="x86" (
    echo Already in x86 VS environment
    goto :build
)

REM Try various VS locations
echo Searching for Visual Studio...

REM VS 2022
for %%e in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" (
        echo Found VS 2022 %%e
        call "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" -arch=x86
        goto :build
    )
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" (
        echo Found VS 2022 %%e
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" -arch=x86
        goto :build
    )
)

REM VS 2019
for %%e in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2019\%%e\Common7\Tools\VsDevCmd.bat" (
        echo Found VS 2019 %%e
        call "%ProgramFiles%\Microsoft Visual Studio\2019\%%e\Common7\Tools\VsDevCmd.bat" -arch=x86
        goto :build
    )
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%e\Common7\Tools\VsDevCmd.bat" (
        echo Found VS 2019 %%e
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\%%e\Common7\Tools\VsDevCmd.bat" -arch=x86
        goto :build
    )
)

REM VS 2017
for %%e in (Community Professional Enterprise BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2017\%%e\Common7\Tools\VsDevCmd.bat" (
        echo Found VS 2017 %%e
        call "%ProgramFiles%\Microsoft Visual Studio\2017\%%e\Common7\Tools\VsDevCmd.bat" -arch=x86
        goto :build
    )
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\%%e\Common7\Tools\VsDevCmd.bat" (
        echo Found VS 2017 %%e
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\%%e\Common7\Tools\VsDevCmd.bat" -arch=x86
        goto :build
    )
)

REM Try using vswhere if available
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo Using vswhere to locate Visual Studio...
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do (
        if exist "%%i\Common7\Tools\VsDevCmd.bat" (
            echo Found VS at %%i
            call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x86
            goto :build
        )
    )
)

echo ERROR: Could not find Visual Studio x86 tools
echo.
echo Please either:
echo 1. Run this from "x86 Native Tools Command Prompt for VS"
echo 2. Or run these commands manually:
echo    - Open any VS developer command prompt
echo    - Run: set VSCMD_ARG_TGT_ARCH=x86
echo    - Run: nmake clean
echo    - Run: nmake
echo.
pause
exit /b 1

:build
echo.
echo Build environment: %VSCMD_ARG_TGT_ARCH%
echo.

if not exist "ftdilib\i386\ftd2xx.lib" (
    echo ERROR: ftdilib\i386\ftd2xx.lib not found!
    echo Please set up the ftdilib folder first.
    pause
    exit /b 1
)
nmake clean
nmake
pause
