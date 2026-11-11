"""Rebuild only the Java, resources and manifest of the Android APK.

Takes the native libraries from an existing APK (default: the one in artifacts/)
so no native toolchain or prepared output/noble-next tree is needed. Use it for
changes under android/app/src/main/{java,res,AndroidManifest.xml}.

    python tools/repack_android_java.py [--base some.apk] [--out out.apk] [--install]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "android" / "app" / "src" / "main"
SDK = Path(os.environ.get("ANDROID_HOME") or os.environ.get("LOCALAPPDATA", "") + "/Android/Sdk")


def run(*args, **kw):
    subprocess.run([str(a) for a in args], check=True, **kw)


def tool(name):
    for bt in sorted((SDK / "build-tools").iterdir(), reverse=True):
        for ext in (".exe", ".bat", ""):
            p = bt / (name + ext)
            if p.exists():
                return p
    raise SystemExit(f"{name} not found in {SDK}/build-tools")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=Path, default=ROOT / "artifacts" / "Noble-Archimedes-0.9.7d-Android-arm64.apk")
    ap.add_argument("--out", type=Path, default=ROOT / "artifacts" / "Noble-Archimedes-Android-repack.apk")
    ap.add_argument("--install", action="store_true")
    args = ap.parse_args()

    java_home = Path(os.environ["JAVA_HOME"])
    android_jar = SDK / "platforms" / "android-34" / "android.jar"
    work = ROOT / "artifacts" / "repack"
    shutil.rmtree(work, ignore_errors=True)
    for d in ("gen", "classes", "dex", "compiled"):
        (work / d).mkdir(parents=True)

    manifest = (MAIN / "AndroidManifest.xml").read_text(encoding="utf-8")
    if "package=" not in manifest:
        # Gradle takes the package from applicationId, and resolves ".Name" classes against the namespace.
        manifest = manifest.replace("<manifest ", '<manifest package="com.noblearchimedes.game" ', 1)
        manifest = manifest.replace('android:name=".', 'android:name="com.noblearchimedes.')
    if "extractNativeLibs" not in manifest:
        manifest = manifest.replace("<application", '<application android:extractNativeLibs="false"', 1)
    (work / "AndroidManifest.xml").write_text(manifest, encoding="utf-8")

    aapt2 = tool("aapt2")
    res = [str(p) for p in (MAIN / "res").rglob("*") if p.is_file()]
    run(aapt2, "compile", "-o", work / "compiled", *res)
    flats = [str(p) for p in (work / "compiled").glob("*.flat")]
    run(aapt2, "link", "-o", work / "base.apk", "-I", android_jar, "--manifest", work / "AndroidManifest.xml",
        "--min-sdk-version", "26", "--target-sdk-version", "34", "--version-code", "908",
        "--version-name", "0.9.7d", "--custom-package", "com.noblearchimedes", "--java", work / "gen", *flats)

    sources = [str(p) for p in (MAIN / "java").rglob("*.java")] + [str(p) for p in (work / "gen").rglob("*.java")]
    run(java_home / "bin" / "javac", "-encoding", "UTF-8", "--release", "17", "-classpath", android_jar,
        "-d", work / "classes", *sources)
    run(java_home / "bin" / "jar", "cf", work / "classes.jar", "-C", work / "classes", ".")
    d8 = next(iter(sorted((SDK / "build-tools").glob("*/lib/d8.jar"), reverse=True)))
    run(java_home / "bin" / "java", "-cp", d8, "com.android.tools.r8.D8", "--release", "--min-api", "26",
        "--lib", android_jar, "--output", work / "dex", work / "classes.jar")

    unaligned = work / "unaligned.apk"
    with zipfile.ZipFile(work / "base.apk") as zbase, zipfile.ZipFile(args.base) as zold, \
            zipfile.ZipFile(unaligned, "w") as out:
        for info in zbase.infolist():
            out.writestr(info, zbase.read(info.filename), compress_type=info.compress_type)
        out.write(work / "dex" / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        for info in zold.infolist():
            if info.filename.startswith("lib/"):
                out.writestr(info.filename, zold.read(info.filename), compress_type=zipfile.ZIP_STORED)

    aligned = work / "aligned.apk"
    run(tool("zipalign"), "-f", "-p", "4", unaligned, aligned)
    ks = Path.home() / ".android" / "debug.keystore"
    run(tool("apksigner"), "sign", "--ks", ks, "--ks-pass", "pass:android", "--key-pass", "pass:android",
        "--ks-key-alias", "androiddebugkey", "--out", args.out, aligned)
    print("built", args.out)
    if args.install:
        adb = SDK / "platform-tools" / "adb.exe"
        run(adb, "install", "-r", args.out)


if __name__ == "__main__":
    sys.exit(main())
