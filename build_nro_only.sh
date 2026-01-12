#!/bin/sh

# Quick build script - NRO only (no payload, no zip)
# Use this for fast development iteration

# builds a preset
build_preset() {
    echo Configuring $1 ...
    cmake --preset $1
    echo Building $1 ...
    cmake --build --preset $1
}

echo "=== Building HATS-Tools NRO only ==="
build_preset Release

echo "=== NRO built: build/Release/hats-tools.nro ==="
