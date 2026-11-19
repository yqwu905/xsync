#!/usr/bin/env bash
set -euo pipefail

QT_DIR="${QT6_DIR:-/home/antigravity/tools/qt/6.8.3/gcc_64}"
BUILD_DIR="${BUILD_DIR:-build}"

cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_DIR"

cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Running xsync test suites..."
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD_DIR" --output-on-failure
echo "Done! Binary: $BUILD_DIR/xsync"
