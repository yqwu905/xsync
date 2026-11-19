#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW_ROOT="${LLVM_MINGW_ROOT:-/home/antigravity/tools/llvm-mingw}"
QT_WIN64_DIR="${QT_WIN64_DIR:-/home/antigravity/tools/qt/6.8.3/llvm-mingw_64}"
QT_HOST_PATH="${QT_HOST_PATH:-/home/antigravity/tools/qt/6.8.3/gcc_64}"
WIN64_DEPS_DIR="${WIN64_DEPS_DIR:-/home/antigravity/tools/win64-deps}"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-win64}"
DIST_DIR="${DIST_DIR:-$ROOT_DIR/dist/xsync-win64}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-llvm-mingw-x64.cmake" \
    -DLLVM_MINGW_ROOT="$LLVM_MINGW_ROOT" \
    -DQT_WIN64_DIR="$QT_WIN64_DIR" \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DWIN64_DEPS_DIR="$WIN64_DEPS_DIR" \
    -DXSYNC_BUILD_TESTS=OFF

cmake --build "$BUILD_DIR" -j"$(nproc)"

# Strip symbols for lean binary size
"$LLVM_MINGW_ROOT/bin/x86_64-w64-mingw32-strip" "$BUILD_DIR/xsync.exe"

# Assemble self-contained portable Windows x64 package
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/platforms" "$DIST_DIR/styles" "$DIST_DIR/iconengines" "$DIST_DIR/imageformats" "$DIST_DIR/tls"

cp "$BUILD_DIR/xsync.exe" "$DIST_DIR/xsync.exe"

# Required Qt6 & LLVM-MinGW UCRT runtime DLLs (OpenSSL 3 + zlib + libssh2 are statically linked!)
for dll in Qt6Core.dll Qt6Gui.dll Qt6Widgets.dll Qt6Network.dll Qt6Svg.dll libc++.dll libunwind.dll; do
    cp "$QT_WIN64_DIR/bin/$dll" "$DIST_DIR/$dll"
done

# Required Qt6 Windows plugins
cp "$QT_WIN64_DIR/plugins/platforms/qwindows.dll" "$DIST_DIR/platforms/"
[ -f "$QT_WIN64_DIR/plugins/styles/qmodernwindowsstyle.dll" ] && \
    cp "$QT_WIN64_DIR/plugins/styles/qmodernwindowsstyle.dll" "$DIST_DIR/styles/"
[ -f "$QT_WIN64_DIR/plugins/iconengines/qsvgicon.dll" ] && \
    cp "$QT_WIN64_DIR/plugins/iconengines/qsvgicon.dll" "$DIST_DIR/iconengines/"
for fmt in qsvg.dll qico.dll qjpeg.dll qgif.dll; do
    [ -f "$QT_WIN64_DIR/plugins/imageformats/$fmt" ] && \
        cp "$QT_WIN64_DIR/plugins/imageformats/$fmt" "$DIST_DIR/imageformats/"
done
[ -f "$QT_WIN64_DIR/plugins/tls/qschannelbackend.dll" ] && \
    cp "$QT_WIN64_DIR/plugins/tls/qschannelbackend.dll" "$DIST_DIR/tls/"

# qt.conf ensures plugins load strictly from the portable folder
cat > "$DIST_DIR/qt.conf" << 'QTCONF'
[Paths]
Plugins = .
QTCONF

mkdir -p "$ROOT_DIR/dist"
ZIP_OUT="$ROOT_DIR/dist/xsync-windows-x64-portable.zip"
rm -f "$ZIP_OUT"
(cd "$DIST_DIR" && 7z a -tzip -mx=9 "$ZIP_OUT" . > /dev/null)

echo "Cross-compilation complete!"
echo "  Executable : $DIST_DIR/xsync.exe"
echo "  Portable   : $ZIP_OUT"
ls -lh "$DIST_DIR/xsync.exe" "$ZIP_OUT"
