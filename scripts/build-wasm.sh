#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build-wasm}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/emsdk}"

if [ ! -d "$EMSDK_DIR" ]; then
    echo "Error: emsdk directory not found at $EMSDK_DIR"
    exit 1
fi

export EMSDK_QUIET=1
source "$EMSDK_DIR/emsdk_env.sh"
export EM_CACHE="${EM_CACHE:-/tmp/emscripten_cache}"

echo "Configuring WASM build in '$BUILD_DIR'..."
FETCH_ARGS=()
if [ -d "build/_deps/tomlplusplus-src" ]; then
    FETCH_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$PWD/build/_deps/tomlplusplus-src")
fi
emcmake cmake -S . -B "$BUILD_DIR" -DZITH_IS_WASM=ON -DCMAKE_C_COMPILER=emcc \
    -DCMAKE_CXX_COMPILER=em++ "${FETCH_ARGS[@]}"

echo "Building WASM target..."
cmake --build "$BUILD_DIR" -j4

echo "Packaging standard library..."
python3 scripts/package-stdlib.py stdlib "$BUILD_DIR/zith-stdlib.pack"

ls -lh "$BUILD_DIR/zith-playground.mjs"
ls -lh "$BUILD_DIR/zith-playground.wasm"
ls -lh "$BUILD_DIR/zith-stdlib.pack"
