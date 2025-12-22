@echo off
cd /d D:\claude_sandbox\phoenix-sdr-net
if exist build rmdir /s /q build
mkdir build
cd build
cmake -G "MinGW Makefiles" ..
cmake --build . --target signal_splitter
