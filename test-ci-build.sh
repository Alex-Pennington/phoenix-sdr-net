#!/usr/bin/env bash
# test-ci-build.sh
# EXACTLY mirrors GitHub Actions workflow
# Run from MSYS2 UCRT64 terminal: ./test-ci-build.sh

set -e

cd "$(dirname "$0")"

echo "=== Simulating GitHub Actions Build ==="
echo ""

# Step 1: Hide SDRplay (GH doesn't have it)
echo "[SETUP] Hiding SDRplayAPI finder to simulate GH environment..."
mv external/phoenix-sdr-core/cmake/FindSDRplayAPI.cmake external/phoenix-sdr-core/cmake/FindSDRplayAPI.cmake.bak 2>/dev/null || true

cleanup() {
    echo "[CLEANUP] Restoring FindSDRplayAPI.cmake..."
    mv external/phoenix-sdr-core/cmake/FindSDRplayAPI.cmake.bak external/phoenix-sdr-core/cmake/FindSDRplayAPI.cmake 2>/dev/null || true
}
trap cleanup EXIT

# Step 2: Clean (fresh like GH checkout)
echo "[CLEAN] Removing build directory..."
rm -rf build

# Step 3: Configure - EXACT same command as GH workflow
echo "[CONFIGURE] cmake --preset msys2-ucrt64"
cmake --preset msys2-ucrt64

# Step 4: Build - EXACT same command as GH workflow  
echo "[BUILD] cmake --build --preset msys2-ucrt64"
cmake --build --preset msys2-ucrt64

# Step 5: Package - EXACT same as GH workflow
echo "[PACKAGE] Creating package directory..."
mkdir -p package
cp build/msys2-ucrt64/sdr_server.exe package/
cp build/msys2-ucrt64/signal_splitter.exe package/
cp README.md package/
cp LICENSE package/
cp -r docs package/

echo ""
echo "=== BUILD SUCCESSFUL ==="
echo ""
ls -la package/
