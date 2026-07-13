@echo off
setlocal

set "RUNTIME_FLAG=/MD"
set "BUILD_LABEL=small (requires Visual C++ Redistributable)"
if /I "%~1"=="standalone" (
    set "RUNTIME_FLAG=/MT"
    set "BUILD_LABEL=standalone"
)

echo ============================================
echo  OutputSwitch - Build Script
echo ============================================
echo  Build mode: %BUILD_LABEL%
echo.

:: Layer 1: Try vswhere
set "VSINSTALL="
set "VSWHE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHE%" set "VSWHE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHE%" (
    echo Searching for MSVC via vswhere...
    for /f "usebackq tokens=*" %%i in (`"%VSWHE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if not "%%i"=="" set "VSINSTALL=%%i"
    )
)
if not defined VSINSTALL if exist "%VSWHE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHE%" -latest -products * -property installationPath 2^>nul`) do (
        if not "%%i"=="" set "VSINSTALL=%%i"
    )
)

:: Layer 2: Try known BuildTools paths
if not defined VSINSTALL (
    for %%d in (
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    ) do (
        if exist "%%~d\Common7\Tools\VsDevCmd.bat" (
            set "VSINSTALL=%%~d"
        )
    )
)

if not defined VSINSTALL (
    echo [ERROR] Visual Studio 2022 Build Tools was not found.
    echo Install "Visual Studio 2022 Build Tools" with the component
    echo "MSVC v143 - VS 2022 C++ x64/x86 build tools".
    pause
    exit /b 1
)

echo VS Path: %VSINSTALL%
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Could not configure the MSVC environment.
    pause
    exit /b 1
)

:: Verify cl.exe is available
where cl.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] cl.exe was not found. Make sure the component
    echo "MSVC v143 - VS 2022 C++ x64/x86 build tools" is installed in Visual Studio Installer.
    pause
    exit /b 1
)

if not exist build mkdir build
if not exist dist  mkdir dist

echo.
echo [1/3] Compiling resources...
rc.exe /nologo /i resources /fo build\OutputSwitch.res resources\OutputSwitch.rc
if %ERRORLEVEL% neq 0 goto :error

echo [2/3] Compiling main.cpp...
cl.exe /nologo /std:c++17 /W4 /WX /O1 /GL /Gy /GS /guard:cf %RUNTIME_FLAG% /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /c /Fo"build\main.obj" "src\main.cpp"
if %ERRORLEVEL% neq 0 goto :error

echo [3/3] Linking...
link.exe /nologo /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /GUARD:CF /OUT:"build\OutputSwitch.exe" "build\main.obj" "build\OutputSwitch.res" Ole32.lib Uuid.lib User32.lib Shell32.lib Advapi32.lib Propsys.lib
if %ERRORLEVEL% neq 0 goto :error

echo.
echo Copying files to dist\ ...
copy /Y build\OutputSwitch.exe dist\OutputSwitch.exe >nul
copy /Y OutputSwitch.ini dist\OutputSwitch.ini >nul

echo.
echo ============================================
echo  Build succeeded!
echo  dist\OutputSwitch.exe
echo ============================================

goto :eof

:error
echo.
echo ============================================
echo  Build failed. Review the errors above.
echo ============================================
exit /b 1
