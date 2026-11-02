"""A release installed and played as a player would: what works on a PC with the build tools should work on one with
nothing but Windows.

usage: python -I tools/cvtest/install.py <release.zip> <game> [<scenario.json>] [--keep]

<game> is the player's copy: a disc image (.iso), or a folder with the extracted disc. The zip (release.bat's) is
extracted into a new folder outside this repository, in TEMP: the game looks for assets and textures in the folders
above its own, so inside the repository it could find this PC's. The copy is linked into its assets (a hard link,
else copied). Then
  - every program and DLL in the package is checked to load: each DLL it imports (delay loaded ones too) is beside
    it, or a part of Windows; the Visual C++ runtime and the like do not count, they are not on a fresh Windows
  - Castlevania.exe is started as from Explorer, in an environment of only Windows' own variables (no build tools
    on PATH, none of this PC's XBE_ and CV_ settings), its first start builds the game code and Start is pressed
  - the scenario plays (scenarios/newgame.json by default), as cvtest.py plays it
The report is cvtest's, in out/test/install-<scenario>-<time>, the checks and the first start before the steps.
The install is deleted after a run that passed, unless --keep.
"""
import os
import shutil
import struct
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # cvtest.py beside this (python -I leaves it off)
import cvtest  # noqa: E402

SYSTEM32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
# in System32 on this PC only because something installed them: a fresh Windows has none
REDISTRIBUTED = ("msvcp", "vcruntime", "concrt", "vccorlib", "vcomp", "ucrtbased", "msvcr", "mfc", "d3dx", "xinput1_",
                 "xaudio2_0", "xaudio2_1", "xaudio2_2", "xaudio2_3", "xaudio2_4", "xaudio2_5", "xaudio2_6",
                 "xaudio2_7", "x3daudio", "xapofx", "d3dcompiler_3", "d3dcompiler_43", "libgcc", "libstdc++",
                 "libwinpthread", "sdl", "python", "openal")
# the variables a fresh Windows session has, without the ones programs add
WINDOWS_VARS = ("ALLUSERSPROFILE", "APPDATA", "CommonProgramFiles", "CommonProgramFiles(x86)", "CommonProgramW6432",
                "COMPUTERNAME", "ComSpec", "HOMEDRIVE", "HOMEPATH", "LOCALAPPDATA", "LOGONSERVER",
                "NUMBER_OF_PROCESSORS", "OS", "PATHEXT", "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER",
                "PROCESSOR_LEVEL", "PROCESSOR_REVISION", "ProgramData", "ProgramFiles", "ProgramFiles(x86)",
                "ProgramW6432", "PUBLIC", "SystemDrive", "SystemRoot", "TEMP", "TMP", "USERDOMAIN", "USERNAME",
                "USERPROFILE", "windir")


def imports(path):
    """the DLLs a PE file imports, delay loaded ones too"""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return []
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional = pe + 24
    size = struct.unpack_from("<H", data, pe + 20)[0]
    plus = struct.unpack_from("<H", data, optional)[0] == 0x20B
    dirs = optional + (112 if plus else 96)
    table = optional + size

    def at(rva):  # file offset of an RVA
        for i in range(sections):
            s = table + i * 40
            vsize, va, raw_size, raw = struct.unpack_from("<IIII", data, s + 8)
            if va <= rva < va + max(vsize, raw_size):
                return raw + rva - va
        return None

    def name(rva):
        o = at(rva)
        return data[o:data.index(b"\0", o)].decode("ascii", "replace") if o is not None else "?"

    found = []
    for index, entry, field in ((1, 20, 12), (13, 32, 4)):  # the import table, the delay load table
        rva = struct.unpack_from("<I", data, dirs + index * 8)[0]
        o = at(rva) if rva else None
        while o is not None and any(data[o:o + entry]):
            found.append(name(struct.unpack_from("<I", data, o + field)[0]))
            o += entry
    return found


def check_package(pkg):
    """each DLL a program or DLL in the package loads: beside it, or part of Windows"""
    errors, seen = [], 0
    built = {"castlevania.dll", "castlevaniaeu.dll"}  # each release's game code, which its first start builds
    for f in sorted(p for p in pkg.rglob("*") if p.suffix.lower() in (".exe", ".dll")):
        seen += 1
        for dll in imports(f):
            low = dll.lower()
            if low.startswith(("api-ms-win-", "ext-ms-")) or (f.parent / dll).exists() or low in built:
                continue
            if (SYSTEM32 / dll).exists() and not low.startswith(REDISTRIBUTED):
                continue
            errors.append(f"{f.relative_to(pkg)} needs {dll}, which is neither beside it nor part of Windows")
    return {"step": "package", "label": f"package: {seen} programs and DLLs load", "ok": not errors,
            "errors": errors, "warnings": []}


def check_contents(pkg):
    """nothing of the game in the download, and what the first start needs is there"""
    errors = [f"game file in the package: {p.relative_to(pkg)}" for p in pkg.rglob("*")
              if p.suffix.lower() in (".xbe", ".iso", ".cpp", ".obj", ".ico")
              or p.name.lower() in ("castlevania.dll", "castlevaniaeu.dll") or p.name.startswith("x86_recomp")]
    # both releases' executables (the USA one starts the European one for its game) and what builds their code
    for need in ("Castlevania.exe", "CastlevaniaEU.exe", "CastlevaniaLauncher.exe", "compiler/xbe_recomp.exe",
                 "compiler/clang.exe", "compiler/lld-link.exe", "compiler/config/game.toml",
                 "compiler/config/game_eu.toml", "compiler/Castlevania.lib", "compiler/CastlevaniaEU.lib", "assets",
                 "textures"):
        if not (pkg / need).exists():
            errors.append(f"missing from the package: {need}")
    return {"step": "package", "label": "package: no game files, the first start's tools there", "ok": not errors,
            "errors": errors, "warnings": []}


def put_game(game, assets):
    """the player's copy into assets: hard links, else copies"""
    def place(src, dst):
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)

    if game.is_dir():
        shutil.copytree(game, assets, dirs_exist_ok=True, copy_function=place)
    else:
        place(game, assets / game.name)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        return 2
    release, game = Path(args[0]).resolve(), Path(args[1]).resolve()
    scenario = Path(args[2]).resolve() if len(args) > 2 else Path(__file__).resolve().parent / "scenarios" / "newgame.json"
    root = Path(tempfile.gettempdir()).resolve() / f"cvtest-install-{time.strftime('%Y%m%d-%H%M%S')}"
    # the game's search for assets and textures goes up to six folders: none of this PC's on the way
    for up in list(root.parents)[:6]:
        for kind in ("assets", "textures"):
            if (up / kind).is_dir():
                print(f"{up / kind} would be found by the installed game; set TEMP to another folder")
                return 2
    with zipfile.ZipFile(release) as z:
        z.extractall(root)
    exes = list(root.rglob("Castlevania.exe"))
    if len(exes) != 1:
        print(f"{release} has {len(exes)} Castlevania.exe")
        return 1
    pkg = exes[0].parent
    print(f"installed into {pkg}")
    checks = [check_contents(pkg), check_package(pkg)]
    put_game(game, pkg / "assets")
    env = {k: os.environ[k] for k in WINDOWS_VARS if k in os.environ}
    windows = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = ";".join([str(SYSTEM32), windows, str(SYSTEM32 / "Wbem"), str(SYSTEM32 / "WindowsPowerShell" / "v1.0")])
    ok = cvtest.run(str(scenario), None, False, install=pkg, env=env, checks=checks)
    if ok and "--keep" not in sys.argv:
        shutil.rmtree(root, ignore_errors=True)  # the game's hard link goes, the player's file stays
    else:
        print(f"kept: {pkg}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
