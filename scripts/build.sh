#!/bin/sh
# DOOM.SGX を組む（2026-11-17）。道具がそろっているかを見てから、CMake の preset を回す。
set -eu
cd "$(dirname "$0")/.."

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "build: arm-none-eabi-gcc がありません（Ubuntu: sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi）" >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "build: cmake がありません" >&2
    exit 1
fi
if ! command -v ninja >/dev/null 2>&1; then
    echo "build: ninja がありません" >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "build: python3 がありません（sgx_link.py が使う）" >&2
    exit 1
fi

cmake --preset sgx "$@"
cmake --build --preset sgx
python3 scripts/check_sgx.py build/sgx/doom.sgx

echo
echo "できました: build/sgx/doom.sgx"
echo "sgnano では DOOM.SGX として WAD と同じドライブに置きます（wads/README.md）"
