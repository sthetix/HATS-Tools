#!/bin/sh

# Get version from CMakeLists.txt
VERSION=$(grep "set(HATS_TOOLS_VERSION" sphaira/CMakeLists.txt | sed 's/.*"\(.*\)".*/\1/')

rm -rf build
rm -rf build/release

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1 || { echo "=== CMake configure FAILED ==="; exit 1; }
    echo Building $1 ...
    cmake --build --preset $1 || { echo "=== Build FAILED ==="; exit 1; }
}

echo "=== Building HATS-Tools NRO ==="
build_preset Release

# Verify NRO was built
if [ ! -f "build/Release/hats-tools.nro" ]; then
    echo "=== ERROR: hats-tools.nro not found! Build may have failed. ==="
    exit 1
fi
echo "=== hats-tools.nro built successfully ==="

rm -rf out

# --- PAYLOAD --- #
echo "=== Building HATS-Installer Payload ==="
(cd payload && make clean && make) || { echo "=== Payload build FAILED ==="; exit 1; }

# Verify payload was built
if [ ! -f "payload/output/hats-installer.bin" ]; then
    echo "=== ERROR: hats-installer.bin not found! Payload build may have failed. ==="
    exit 1
fi
echo "=== hats-installer.bin built successfully ==="

# --- PACKAGE --- #
mkdir -p out/switch/hats-tools/
mkdir -p out/config/hats-tools/

cp build/Release/hats-tools.nro out/switch/hats-tools/hats-tools.nro
cp payload/output/hats-installer.bin out/switch/hats-tools/hats-installer.bin
cp assets/romfs/hekate_ipl_mod.ini out/config/hats-tools/hekate_ipl_mod.ini

pushd out
zip -r9 hats-tools-$VERSION.zip switch config
popd

echo "=== Release built: out/hats-tools-$VERSION.zip ==="
