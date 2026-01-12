#!/bin/sh

# Get version from CMakeLists.txt
VERSION=$(grep "set(HATS_TOOLS_VERSION" sphaira/CMakeLists.txt | sed 's/.*"\(.*\)".*/\1/')

rm -rf build
rm -rf build/release

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1
    echo Building $1 ...
    cmake --build --preset $1
}

echo "=== Building HATS-Tools NRO ==="
build_preset Release

rm -rf out

# --- PAYLOAD --- #
echo "=== Building HATS-Installer Payload ==="
(cd payload && make clean && make)

# --- PACKAGE --- #
mkdir -p out/switch/hats-tools/
mkdir -p out/bootloader/payloads/

cp build/Release/hats-tools.nro out/switch/hats-tools/hats-tools.nro
cp payload/output/hats-installer.bin out/bootloader/payloads/hats-installer.bin

pushd out
zip -r9 hats-tools-$VERSION.zip switch bootloader
popd

echo "=== Release built: out/hats-tools-$VERSION.zip ==="
