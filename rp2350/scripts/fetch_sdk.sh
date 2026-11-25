#!/bin/bash
# fetch_sdk.sh - get the Pico SDK 2.2.0 the firmware builds against.
#
# The SDK is ~63 MB and is a stock upstream download, so it is not committed.
# Run this once after cloning; after that `cmake -S . -B build` finds it.
#
# On this machine use the USTC mirror first (fastest of five measured):
# mirrors.ustc.edu.cn > tuna > nju > repo.msys2.org > aliyun.

set -e
cd "$(dirname "$0")"

SDK_DIR="pico-sdk-2.2.0"
SDK_VER="2.2.0"

if [ -d "$SDK_DIR/lib/tinyusb/hw" ]; then
    echo "Pico SDK $SDK_VER already present in $SDK_DIR/"
    exit 0
fi

echo "=== Fetching Pico SDK $SDK_VER ==="

# 1) Try a local copy first - the reference tree usually has one.
for CAND in \
    "/e/working/vscode-projects/YM2163-Midi/RPFM/pico-sdk-2.2.0" \
    "$HOME/pico-sdk-2.2.0"
do
    if [ -d "$CAND/lib/tinyusb/hw" ]; then
        echo "copying from $CAND"
        cp -r "$CAND" .
        echo "done"
        exit 0
    fi
done

# 2) Otherwise clone from GitHub (through the accelerator if set).
URL="${PICO_SDK_URL:-https://github.com/raspberrypi/pico-sdk}"
grep -q "^URL" /dev/null 2>/dev/null || true

if [ -n "$GH_PROXY" ]; then
    URL="${GH_PROXY}${URL}"
    echo "using proxy: $URL"
fi

git clone --depth 1 --branch "$SDK_VER" --recurse-submodules "$URL" "$SDK_DIR" \
    || { echo "clone failed; set GH_PROXY or clone manually into $SDK_DIR/"; exit 1; }

echo "done"
