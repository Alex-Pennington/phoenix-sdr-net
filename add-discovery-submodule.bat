@echo off
REM Run this in the phoenix-sdr-net directory to add the submodule

cd /d D:\claude_sandbox\phoenix-sdr-net

REM Remove any leftover broken state
if exist external\phoenix-discovery rmdir /s /q external\phoenix-discovery

REM Add the submodule fresh
git submodule add https://github.com/Alex-Pennington/phoenix-discovery.git external/phoenix-discovery

REM Verify
git submodule status

echo.
echo Done. Now run:
echo   git add .gitmodules external/phoenix-discovery
echo   git commit -m "Add phoenix-discovery submodule"
