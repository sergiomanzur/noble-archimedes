#!/bin/bash
# Build the arm64 vkd3d 2.1 libraries used by the Android APK.
# Run from an MSYS2 bash with bison, flex, make, pkgconf, perl-JSON, coreutils,
# tar and xz installed. NDK must be 26.2 or a compatible NDK with Vulkan headers.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/AppData/Local/Android/Sdk/ndk/26.2.11394342}
TOOL=$NDK/toolchains/llvm/prebuilt/windows-x86_64
SYSROOT=$TOOL/sysroot
WORK=${WORK:-$ROOT/output/android-work}
SRC=$WORK/vkd3d-2.1
BUILD=$WORK/vkd3d-build
OUT=$WORK/vkd3d-install
SPIRV=$ROOT/android/third_party/spirv

command -v bison >/dev/null || { echo "install bison (pacman -S bison)"; exit 1; }
command -v flex >/dev/null || { echo "install flex (pacman -S flex)"; exit 1; }
command -v make >/dev/null || { echo "install make (pacman -S make)"; exit 1; }
command -v pkg-config >/dev/null || { echo "install pkgconf (pacman -S pkgconf)"; exit 1; }
[ -x "$TOOL/bin/clang.exe" ] || { echo "NDK clang not found at $TOOL"; exit 1; }

mkdir -p "$WORK"
if [ ! -f "$WORK/vkd3d-2.1.tar.xz" ]; then
    curl -fsSL https://dl.winehq.org/vkd3d/source/vkd3d-2.1.tar.xz -o "$WORK/vkd3d-2.1.tar.xz"
fi
if [ ! -d "$SRC" ]; then
    tar -xf "$WORK/vkd3d-2.1.tar.xz" -C "$WORK"
fi
cp "$ROOT/android/third_party/vkd3d/patches/device.c" "$SRC/libs/vkd3d/device.c"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

export CC="$TOOL/bin/clang.exe --target=aarch64-linux-android26 --sysroot=$SYSROOT -fPIC -O2"
export AR=$TOOL/bin/llvm-ar.exe
export RANLIB=$TOOL/bin/llvm-ranlib.exe
export STRIP=$TOOL/bin/llvm-strip.exe
export READELF=$TOOL/bin/llvm-readelf.exe
export SONAME_LIBVULKAN=libvulkan.so
export CPPFLAGS="-I$SYSROOT/usr/include -I$SPIRV"
export LDFLAGS="-L$SYSROOT/usr/lib/aarch64-linux-android/26 -L$SYSROOT/usr/lib/aarch64-linux-android"

"$SRC/configure" \
    --host=aarch64-linux-android \
    --build=x86_64-pc-msys \
    --prefix="$OUT" \
    --disable-tests --disable-demos \
    --without-spirv-tools --without-ncurses --without-xcb --without-opengl

make -j"$(nproc)"
make install

mkdir -p "$ROOT/android/third_party/vkd3d/lib" "$ROOT/android/third_party/vkd3d/include"
cp "$OUT/lib/libvkd3d.so" "$OUT/lib/libvkd3d-shader.so" "$OUT/lib/libvkd3d-utils.so" \
   "$ROOT/android/third_party/vkd3d/lib/"
rm -rf "$ROOT/android/third_party/vkd3d/include/vkd3d"
cp -R "$OUT/include/vkd3d" "$ROOT/android/third_party/vkd3d/include/"
echo "Updated android/third_party/vkd3d"
