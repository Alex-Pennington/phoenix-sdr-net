@echo off
setlocal

set "MINGW=C:\Users\rayve\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
set "CMAKE=%MINGW%\cmake.exe"

cd /d D:\claude_sandbox\phoenix-sdr-net

echo === Phoenix SDR Net - CI Build Test ===
echo.
echo Using cmake: %CMAKE%
echo.

REM Step 1: Hide SDRplay finder (GH doesn't have it)
echo [1/5] Hiding SDRplayAPI finder...
if exist "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake" (
    move /Y "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake" "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake.bak" >nul
)

REM Step 2: Clean
echo [2/5] Cleaning build directory...
if exist "build" rmdir /s /q "build"

REM Step 3: Configure
echo [3/5] Configuring...
"%CMAKE%" --preset msys2-ucrt64
if errorlevel 1 goto :fail

REM Step 4: Build
echo [4/5] Building...
"%CMAKE%" --build --preset msys2-ucrt64
if errorlevel 1 goto :fail

REM Step 5: Package
echo [5/5] Packaging...
if not exist "package" mkdir package
copy /Y "build\msys2-ucrt64\sdr_server.exe" "package\" >nul
copy /Y "build\msys2-ucrt64\signal_splitter.exe" "package\" >nul
copy /Y "README.md" "package\" >nul
copy /Y "LICENSE" "package\" >nul
xcopy /E /I /Y "docs" "package\docs" >nul

echo.
echo === BUILD SUCCESSFUL ===
echo.
dir package\*.exe
goto :cleanup

:fail
echo.
echo === BUILD FAILED ===
goto :cleanup

:cleanup
echo.
echo Restoring FindSDRplayAPI.cmake...
if exist "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake.bak" (
    move /Y "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake.bak" "external\phoenix-sdr-core\cmake\FindSDRplayAPI.cmake" >nul
)
