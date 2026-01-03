#!/bin/sh

rm -rf build
rm -rf build/release

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1
    echo Building $1 ...
    cmake --build --preset $1
}

build_preset Release

rm -rf out


# --- SWITCH --- #
mkdir -p out/switch/hats-tools/
cp build/Release/hats-tools.nro out/switch/hats-tools/hats-tools.nro
pushd out
zip -r9 hats-tools.zip switch
popd
