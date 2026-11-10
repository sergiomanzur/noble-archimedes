#!/usr/bin/env python3
"""Build the Noble Archimedes Android APK from a prepared advanced PC tree.

Prerequisites:
  * a prepared advanced runtime in output/noble-next (run
    `python xbox/tools/noble_next.py --assets extracted --build --full-game`
    once on the PC);
  * Android SDK with NDK 26.2 and CMake 3.22;
  * JDK 17 or 21 for Gradle;
  * Gradle 8.10.x (the Android Studio cache or a `gradle` on PATH).

The script copies the Android overlay over the prepared SDK/port, generates
the per-XBE support files, and runs Gradle. It never modifies `output/noble-next`
outside the overlay files themselves.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
ANDROID = ROOT / "android"
NEXT = ROOT / "output" / "noble-next"
DEFAULT_SDK = NEXT / "sdk"
DEFAULT_PORT = NEXT / "port"
DEFAULT_GENERATED = DEFAULT_PORT / "generated" / "game"
DEFAULT_VKD3D = ANDROID / "third_party" / "vkd3d"
OVERLAY = ANDROID / "overlay"


def fail(message: str) -> None:
    raise SystemExit(f"build_android: {message}")


def run(args, **kwargs) -> None:
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def java_major(java: Path) -> int | None:
    try:
        out = subprocess.check_output([str(java), "-version"], stderr=subprocess.STDOUT, text=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    found = re.search(r'version "(\d+)', out)
    return int(found.group(1)) if found else None


def find_java() -> Path:
    candidates = []
    if os.environ.get("JAVA_HOME"):
        candidates.append(Path(os.environ["JAVA_HOME"]) / "bin" / "java.exe")
    candidates += [
        Path(r"C:\Program Files\Android\Android Studio\jbr\bin\java.exe"),
        ROOT / "output" / "android-work" / "jdk" / "jdk-17.0.21+9" / "bin" / "java.exe",
    ]
    candidates += sorted((ROOT / "output").glob("android-work/jdk/*/bin/java.exe"))
    for java in candidates:
        if java.is_file() and (java_major(java) or 0) in (17, 18, 19, 20, 21):
            return java
    fail("no JDK 17 or 21 found (Android Studio's Java 25 is too new for Gradle 8.10); "
         "set JAVA_HOME to a JDK 17/21")


def find_gradle(java_home: Path) -> Path:
    if os.environ.get("GRADLE"):
        p = Path(os.environ["GRADLE"])
        if p.is_file():
            return p
    wrapper = ANDROID / ("gradlew.bat" if os.name == "nt" else "gradlew")
    if wrapper.is_file():
        return wrapper
    found = shutil.which("gradle")
    if found:
        return Path(found)
    home = Path.home() / ".gradle" / "wrapper" / "dists"
    for pattern in ("gradle-8.10.2-bin/*/gradle-8.10.2/bin/gradle.bat",
                    "gradle-8.10.2-bin/*/gradle-8.10.2/bin/gradle",
                    "gradle-8.9-bin/*/gradle-8.9/bin/gradle.bat",
                    "gradle-8.7-bin/*/gradle-8.7/bin/gradle.bat"):
        for p in sorted(home.glob(pattern)):
            return p
    fail("no Gradle found; install Gradle 8.10.x or set GRADLE to its launcher")


def find_sdk() -> Path:
    for key in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(key):
            p = Path(os.environ[key])
            if (p / "platform-tools").is_dir():
                return p
    p = Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk"
    if (p / "platform-tools").is_dir():
        return p
    fail("Android SDK not found; set ANDROID_HOME")


def copy_overlay(sdk: Path, port: Path) -> None:
    sdk_overlay = OVERLAY / "sdk"
    port_overlay = OVERLAY / "port"
    if not sdk_overlay.is_dir() or not port_overlay.is_dir():
        fail("android/overlay is missing")
    shutil.copytree(sdk_overlay, sdk, dirs_exist_ok=True)
    shutil.copytree(port_overlay, port, dirs_exist_ok=True)


def write_xbe_support(assets: Path, generated: Path) -> None:
    xbe_path = assets / "default.xbe"
    if not xbe_path.is_file():
        fail(f"missing {xbe_path}")
    xbe = xbe_path.read_bytes()
    base = struct.unpack_from("<I", xbe, 0x104)[0]
    cert = struct.unpack_from("<I", xbe, 0x118)[0] - base
    title = xbe[cert + 0x0C:cert + 0x0C + 80]
    media = xbe[cert + 0x9C:cert + 0x9C + 12]
    sha = hashlib.sha1(xbe).hexdigest()
    generated.mkdir(parents=True, exist_ok=True)
    (generated / "xbe_hash.cpp").write_text(
        f'extern const char kImageSha1[] = "{sha}";\n'
        f'extern const unsigned char kImageCertificate[92] = {{'
        + ",".join(f"0x{b:02x}" for b in title + media) + '};\n',
        encoding="utf-8", newline="\n")
    (generated / "releases.cpp").write_text(
        "// the game's releases, written by tools/build_android.py\n"
        '#include "app/app.h"\n\n'
        "namespace {\n"
        "const unsigned char kCertificate0[92] = {"
        + ",".join(f"0x{b:02x}" for b in title + media) + "};\n"
        "}  // namespace\n\n"
        "extern const app::GameInfo::Release kReleases[] = {\n"
        f'    {{"{sha}", kCertificate0, "NobleArchimedes"}},\n'
        "};\n"
        "extern const unsigned kReleaseCount = 1;\n",
        encoding="utf-8", newline="\n")
    print(f"XBE support: {sha}")


def generate_game_code(port: Path, sdk: Path) -> None:
    out = port / "generated" / "game"
    if (out / "x86_recomp.0.cpp").is_file():
        return
    tool = NEXT / "build-windows" / "compiler" / "xbe_recomp.exe"
    if not tool.is_file():
        tool = NEXT / "build-linux" / "compiler" / "xbe_recomp"
    if not tool.is_file():
        fail("game code is not generated and xbe_recomp is missing; run "
             "`python xbox/tools/noble_next.py --assets extracted --build --full-game` first")
    assets = port / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    if not (assets / "default.xbe").is_file():
        shutil.copy2(ROOT / "extracted" / "default.xbe", assets / "default.xbe")
    run([tool, "config/game.toml", "--xdk", sdk / "xdk"], cwd=port)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ROOT / "extracted")
    parser.add_argument("--sdk", type=Path, default=DEFAULT_SDK)
    parser.add_argument("--port", type=Path, default=DEFAULT_PORT)
    parser.add_argument("--generated", type=Path, default=DEFAULT_GENERATED)
    parser.add_argument("--vkd3d", type=Path, default=DEFAULT_VKD3D)
    parser.add_argument("--imgui", type=Path, default=NEXT / "build-windows" / "_deps" / "imgui-src")
    parser.add_argument("--toml", type=Path, default=NEXT / "build-windows" / "_deps" / "tomlplusplus-src")
    parser.add_argument("--xxhash", type=Path, default=NEXT / "build-windows" / "_deps" / "xxhash-src")
    parser.add_argument("--sdl", type=Path, default=NEXT / "build-windows" / "_deps" / "sdl3-src")
    parser.add_argument("--stb", type=Path, default=ANDROID / "third_party" / "stb")
    parser.add_argument("--release", action="store_true", help="build the release APK (debug-key signed)")
    parser.add_argument("--install", action="store_true", help="install with adb after the build")
    parser.add_argument("--no-overlay", action="store_true", help="keep the prepared tree as it is")
    parser.add_argument("--regenerate", action="store_true", help="force game-code regeneration")
    args = parser.parse_args()

    sdk, port, generated = args.sdk.resolve(), args.port.resolve(), args.generated.resolve()
    for p in (sdk, port):
        if not p.is_dir():
            fail(f"{p} is missing; prepare output/noble-next first")
    if not (sdk / "runtime" / "cpu" / "dispatch.cpp").is_file() or not (port / "src" / "common" / "game_info.cpp").is_file():
        fail("the prepared SDK/port layout is not the advanced runtime")

    if args.stb.is_dir() is False:
        fail(f"stb headers missing at {args.stb}; see android/README.md")
    if not args.imgui.is_dir() or not args.toml.is_dir() or not args.xxhash.is_dir() or not args.sdl.is_dir():
        fail("the advanced PC dependencies are missing; run the advanced build once on this machine")

    if not args.no_overlay:
        copy_overlay(sdk, port)
        print("Applied android/overlay to the prepared SDK/port")

    if args.regenerate:
        shutil.rmtree(generated, ignore_errors=True)
    generate_game_code(port, sdk)
    write_xbe_support(args.assets.resolve(), generated)

    vkd3d = args.vkd3d.resolve()
    if not (vkd3d / "lib" / "libvkd3d.so").is_file():
        fail(f"vkd3d prebuilts missing at {vkd3d}; see android/third_party/vkd3d/README.md")

    sdk_root = find_sdk()
    java = find_java()
    java_home = java.parent.parent
    gradle = find_gradle(java_home)
    gradle_env = os.environ.copy()
    gradle_env["JAVA_HOME"] = str(java_home)
    gradle_env["ANDROID_HOME"] = str(sdk_root)
    gradle_env["ANDROID_SDK_ROOT"] = str(sdk_root)

    task = ":app:assembleRelease" if args.release else ":app:assembleDebug"
    paths = {
        "nobleSdk": sdk,
        "noblePort": port,
        "nobleVkd3d": vkd3d,
        "nobleGenerated": generated,
        "nobleImgui": args.imgui.resolve(),
        "nobleToml": args.toml.resolve(),
        "nobleXxhash": args.xxhash.resolve(),
        "nobleSdl": args.sdl.resolve(),
        "nobleStb": args.stb.resolve(),
    }
    run([gradle, "-p", ANDROID, task, "--console=plain",
         *[f"-P{k}={v.as_posix()}" for k, v in paths.items()]], env=gradle_env)

    variant = "release" if args.release else "debug"
    apk = ANDROID / "app" / "build" / "outputs" / "apk" / variant / f"app-{variant}.apk"
    print(f"APK: {apk}", flush=True)
    if args.install:
        adb = sdk_root / "platform-tools" / "adb.exe"
        run([adb, "install", "-r", apk])


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(f"build_android: command failed with {error.returncode}")
