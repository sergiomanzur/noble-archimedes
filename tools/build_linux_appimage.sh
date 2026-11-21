#!/usr/bin/env bash
# Build the portable Noble Archimedes Linux x86_64 AppImage in the Ubuntu 22.04
# (glibc 2.35) Podman container.
#
# Usage (inside Linux or WSL2 with podman + mksquashfs):
#   bash tools/build_linux_appimage.sh [output_appimage_path]
#
# Optional environment variables:
#   NOBLE_LINUX_WORK   native Linux work/cache dir (default: $HOME/.cache/noble-linux-release)
#   REUSE_APPIMAGE     path to an existing Noble Archimedes AppImage to reuse its
#                      bundled clang/ld.lld, P052 fonts and Type-2 AppImage runtime
set -euo pipefail

VKD3D=2.1
FFMPEG=7.1.2
LLVM=23.1.4
URW_FONTS=20200910

REPO="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(tr -d '\r\n' < "$REPO/VERSION")"
OUT_APPIMAGE="${1:-$REPO/output/releases/Noble-Archimedes-${VERSION}-Linux-x86_64.AppImage}"
WORK="${NOBLE_LINUX_WORK:-$HOME/.cache/noble-linux-release}"
CACHE="$WORK/cache"
BUILD_WORK="$WORK/work"
NEXT_WORK="$WORK/noble-next-linux"

mkdir -p "$CACHE" "$BUILD_WORK" "$(dirname "$OUT_APPIMAGE")"

if [ ! -f "$REPO/extracted/default.xbe" ]; then
    echo "Missing $REPO/extracted/default.xbe" >&2
    exit 1
fi

echo "[1/5] Preparing Linux advanced port + SDK (with AMD GCN fix & GitHub auto-updater)"
NOBLE_NEXT_WORK="$NEXT_WORK" python3 "$REPO/xbox/tools/noble_next.py" --assets "$REPO/extracted"

PORT="$NEXT_WORK/port"
SDK="$NEXT_WORK/sdk"

# Verify the Ubuntu 22.04 build container image exists (build it if not).
if ! podman image exists localhost/cvcod-linux-build:latest; then
    echo "[2/5] Building Ubuntu 22.04 container image (cvcod-linux-build)"
    podman build -t localhost/cvcod-linux-build:latest -f "$PORT/tools/linux/Containerfile" "$PORT/tools/linux"
else
    echo "[2/5] Using existing container image localhost/cvcod-linux-build:latest"
fi

# Optionally seed clang/ld.lld, P052 fonts, and AppImage runtime from an existing AppImage.
if [ -n "${REUSE_APPIMAGE:-}" ] && [ -f "$REUSE_APPIMAGE" ]; then
    echo "      Seeding runtime/clang/fonts from $REUSE_APPIMAGE"
    chmod +x "$REUSE_APPIMAGE" || true
    OFFSET="$("$REUSE_APPIMAGE" --appimage-offset)"
    if [ ! -f "$CACHE/runtime-x86_64" ]; then
        head -c "$OFFSET" "$REUSE_APPIMAGE" > "$CACHE/runtime-x86_64"
    fi
    SEED_DIR="$BUILD_WORK/seed-extract"
    if [ ! -x "$BUILD_WORK/llvm/ld.lld" ] || [ ! -f "$CACHE/P052-Roman.otf" ]; then
        rm -rf "$SEED_DIR" && mkdir -p "$SEED_DIR"
        (cd "$SEED_DIR" && "$REUSE_APPIMAGE" --appimage-extract >/dev/null)
        mkdir -p "$BUILD_WORK/llvm"
        cp -f "$SEED_DIR/squashfs-root/compiler/clang" "$BUILD_WORK/llvm/clang"
        cp -f "$SEED_DIR/squashfs-root/compiler/ld.lld" "$BUILD_WORK/llvm/ld.lld"
        chmod +x "$BUILD_WORK/llvm/clang" "$BUILD_WORK/llvm/ld.lld"
        cp -f "$SEED_DIR/squashfs-root/fonts"/P052-*.otf "$CACHE/"
        if [ -f "$SEED_DIR/squashfs-root/THIRD_PARTY.txt" ]; then
            cp -f "$SEED_DIR/squashfs-root/THIRD_PARTY.txt" "$CACHE/THIRD_PARTY-seed.txt"
        fi
        rm -rf "$SEED_DIR"
    fi
fi

if [ ! -f "$CACHE/runtime-x86_64" ]; then
    curl -fsSL -o "$CACHE/runtime-x86_64" \
        "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64"
fi

cat > "$BUILD_WORK/inside_build.sh" <<'INNER'
#!/usr/bin/env bash
set -euo pipefail
VKD3D=2.1
FFMPEG=7.1.2
LLVM=23.1.4
URW_FONTS=20200910
SRC=/src/port
SDK=/src/sdk
export CC=clang-21 CXX=clang++-21

echo "[3/5] Building vkd3d $VKD3D and FFmpeg $FFMPEG inside Ubuntu 22.04"
cd /cache
[ -f vkd3d-$VKD3D.tar.xz ] || curl -fsSLO https://dl.winehq.org/vkd3d/source/vkd3d-$VKD3D.tar.xz
[ -f ffmpeg-$FFMPEG.tar.xz ] || curl -fsSLO https://ffmpeg.org/releases/ffmpeg-$FFMPEG.tar.xz
URW=https://github.com/ArtifexSoftware/urw-base35-fonts/raw/$URW_FONTS
for f in Roman Bold BoldItalic; do [ -f P052-$f.otf ] || curl -fsSLO $URW/fonts/P052-$f.otf; done
for f in LICENSE COPYING; do [ -f urw-base35-$f.txt ] || curl -fsSL -o urw-base35-$f.txt $URW/$f; done

if [ ! -f /work/vkd3d/lib/libvkd3d.so.1 ]; then
    rm -rf /work/vkd3d-src && mkdir -p /work/vkd3d-src
    tar xf vkd3d-$VKD3D.tar.xz -C /work/vkd3d-src --strip-components=1
    (cd /work/vkd3d-src && ./configure -q --prefix=/work/vkd3d --disable-tests --disable-demos --disable-doxygen-doc \
        CFLAGS="-O2 -march=x86-64-v2" > /work/vkd3d-configure.log 2>&1 &&
        make -s -j"$(nproc)" > /work/vkd3d-make.log 2>&1 &&
        make -s install > /dev/null)
fi

if [ ! -f /work/ffmpeg/lib/libavcodec.so ]; then
    rm -rf /work/ffmpeg-src && mkdir -p /work/ffmpeg-src
    tar xf ffmpeg-$FFMPEG.tar.xz -C /work/ffmpeg-src --strip-components=1
    (cd /work/ffmpeg-src && ./configure --prefix=/work/ffmpeg --cc=clang-21 --extra-cflags=-march=x86-64-v2 \
        --enable-shared --disable-static --enable-pic --disable-programs --disable-doc --disable-network \
        --disable-autodetect --disable-everything --disable-avdevice --disable-avfilter --disable-swresample \
        --disable-postproc --enable-decoder=h264,hevc --enable-parser=h264,hevc --enable-demuxer=mov \
        --enable-protocol=file --enable-swscale > /work/ffmpeg-configure.log 2>&1 &&
        make -j"$(nproc)" > /work/ffmpeg-make.log 2>&1 && make install > /dev/null 2>&1)
fi

if [ ! -x /work/llvm/ld.lld ]; then
    [ -f LLVM-$LLVM-Linux-X64.tar.zst ] || curl -fsSLO https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM/LLVM-$LLVM-Linux-X64.tar.zst
    rm -rf /work/llvm && mkdir -p /work/llvm
    root=LLVM-$LLVM-Linux-X64
    clang=$(zstd -dq --long=31 -c LLVM-$LLVM-Linux-X64.tar.zst | tar -tf - | grep -E "^$root/bin/clang-[0-9]+$" | head -1)
    zstd -dq --long=31 -c LLVM-$LLVM-Linux-X64.tar.zst | tar -xf - -C /work/llvm --strip-components=2 "$clang" "$root/bin/lld"
    mv /work/llvm/"$(basename "$clang")" /work/llvm/clang && mv /work/llvm/lld /work/llvm/ld.lld
    llvm-strip-21 /work/llvm/clang /work/llvm/ld.lld
fi

echo "[4/5] Compiling NobleArchimedes, NobleArchimedesLauncher, xbe_recomp, NobleArchimedes_hooks"
B=/work/build
LINK="-Wl,--disable-new-dtags"
cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
    -DCMAKE_EXE_LINKER_FLAGS="$LINK" -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DFETCHCONTENT_SOURCE_DIR_XBE_SDK="$SDK" \
    -DNOBLE_BRANDING_ASSETS=/branding \
    -DXBE_VKD3D_DIR=/work/vkd3d \
    -DCV_FFMPEG_DIR=/work/ffmpeg \
    -DCV_GAME_ICON=OFF
cmake --build "$B" --parallel "$(nproc)" --target NobleArchimedes NobleArchimedesLauncher xbe_recomp NobleArchimedes_hooks

APPDIR=/work/AppDir
rm -rf "$APPDIR" && mkdir -p "$APPDIR/assets" "$APPDIR/textures" "$APPDIR/lib" "$APPDIR/fonts" "$APPDIR/branding"
cp "$B/NobleArchimedes" "$B/NobleArchimedesLauncher" "$APPDIR/"
for l in libvkd3d.so.1 libvkd3d-shader.so.1 libvkd3d-utils.so.1; do
    cp -L "/work/vkd3d/lib/$l" "$APPDIR/lib/"
done
for l in avformat avcodec avutil swscale; do
    cp -L /work/ffmpeg/lib/$(cd /work/ffmpeg/lib && ls lib$l.so.* | awk -F. 'NF == 3') "$APPDIR/lib/"
done
patchelf --set-rpath '$ORIGIN' "$APPDIR/lib"/*.so.*

(cd "$B/compiler" && tar cf - --exclude=./work .) | (cd "$APPDIR" && mkdir compiler && tar xf - -C compiler)
cp /work/llvm/clang /work/llvm/ld.lld "$APPDIR/compiler/"
cp "$B/fonts/MaterialIcons-Regular.ttf" /cache/P052-*.otf "$APPDIR/fonts/"
cp -a /branding/. "$APPDIR/branding/"
cp "$APPDIR/branding/noble-icon.png" "$APPDIR/noble-archimedes.png"
ln -sf noble-archimedes.png "$APPDIR/.DirIcon"

echo "$VERSION" > "$APPDIR/version.txt"
cp "$SRC/LICENSE" "$APPDIR/LICENSE.txt"
cp /repo/release/linux/README-${VERSION}.txt "$APPDIR/README.txt"
cat > "$APPDIR/README-APPIMAGE.txt" <<EOF
Noble Archimedes ${VERSION} - Linux x86_64 AppImage

Put your own Xbox game in assets/: the disc image (.iso) or the extracted
disc (default.xbe with data, movie and sound folders). The first run copies
this package to ~/.local/share/NobleArchimedes (or \$XDG_DATA_HOME) so the
game has a writable folder for assets, saves, settings and logs. Put the game
and HD texture packs in that copy after the first start.

Requirements: a 64-bit Linux with glibc 2.35 or later, Vulkan drivers, and
about 0.6 GB of free space besides the game. The package includes vkd3d,
FFmpeg and the compiler used for the first-run game build.
EOF

cat "$SDK/THIRD_PARTY.txt" "$SRC/tools/release/THIRD_PARTY-port.txt" "$SRC/tools/linux/THIRD_PARTY-linux.txt" \
    /cache/urw-base35-LICENSE.txt /cache/urw-base35-COPYING.txt \
    /work/ffmpeg-src/COPYING.LGPLv2.1 > "$APPDIR/THIRD_PARTY.txt"

echo "Put your game here: its disc image (.iso), or the extracted disc (default.xbe with its folders)." > "$APPDIR/assets/PUT YOUR GAME HERE.txt"
echo "Optional: an HD texture pack made for PCSX2. Put its replacement pictures here (.png or .dds files, folders inside are fine)." > "$APPDIR/textures/PUT A TEXTURE PACK HERE.txt"

cat > "$APPDIR/noble-archimedes.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Noble Archimedes
Comment=Castlevania: Curse of Darkness, native recompilation
Exec=NobleArchimedesLauncher
Icon=noble-archimedes
Terminal=false
Categories=Game;
StartupNotify=false
EOF

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
set -e
HERE=$(dirname "$(readlink -f "$0")")
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/NobleArchimedes"
if [ ! -x "$DATA/NobleArchimedesLauncher" ]; then
    mkdir -p "$DATA"
    cp -a "$HERE/." "$DATA/"
    chmod +x "$DATA/NobleArchimedes" "$DATA/NobleArchimedesLauncher" \
        "$DATA/compiler/xbe_recomp" "$DATA/compiler/clang" "$DATA/compiler/ld.lld" 2>/dev/null || true
fi
cd "$DATA"
exec "$DATA/NobleArchimedesLauncher" "$@"
EOF

find "$APPDIR" -type f -exec chmod 644 {} +
find "$APPDIR" -type d -exec chmod 755 {} +
chmod 755 "$APPDIR/AppRun" "$APPDIR/NobleArchimedes" "$APPDIR/NobleArchimedesLauncher" \
    "$APPDIR/compiler/xbe_recomp" "$APPDIR/compiler/clang" "$APPDIR/compiler/ld.lld"

# Verify no game data leaked into the AppDir.
bad=$(find "$APPDIR" \( -name '*.xbe' -o -name '*.iso' -o -name '*.cpp' -o -name '*.o' -o -name 'x86_recomp*' -o \
    -name image_symbols.h -o -name NobleArchimedes.so -o -name NobleArchimedesEU.so \) ! -path "$APPDIR/compiler/ngx/cv_ngx.cpp" -print)
[ -z "$bad" ] || { echo "Game files leaked into AppDir: $bad" >&2; exit 1; }
[ ! -e "$APPDIR/compiler/work" ] || { echo "compiler/work leaked into AppDir" >&2; exit 1; }
INNER
chmod +x "$BUILD_WORK/inside_build.sh"

podman run --rm \
    -e "VERSION=$VERSION" \
    -v "$PORT:/src/port:ro" \
    -v "$SDK:/src/sdk:ro" \
    -v "$REPO:/repo:ro" \
    -v "$REPO/assets/noble:/branding:ro" \
    -v "$CACHE:/cache" \
    -v "$BUILD_WORK:/work" \
    localhost/cvcod-linux-build:latest bash /work/inside_build.sh

echo "[5/5] Packing Type-2 AppImage (zstd SquashFS)"
rm -f "$BUILD_WORK/squash.img"
mksquashfs "$BUILD_WORK/AppDir" "$BUILD_WORK/squash.img" -root-owned -noappend -comp zstd -quiet
cat "$CACHE/runtime-x86_64" "$BUILD_WORK/squash.img" > "$OUT_APPIMAGE.tmp"
chmod 755 "$OUT_APPIMAGE.tmp"
mv "$OUT_APPIMAGE.tmp" "$OUT_APPIMAGE"
echo "Built $OUT_APPIMAGE ($(stat -c %s "$OUT_APPIMAGE") bytes)"
