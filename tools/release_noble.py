"""Build a clean, self-contained runtime ZIP from explicit public inputs.

Never traverses the build tree for runtime payloads, never deletes old releases,
and refuses to finalize when a required input is absent. Game code is compiled
on the user's machine from their own disc; precompiled game DLLs are excluded.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid
import zipfile

VERSION = (Path(__file__).resolve().parents[1] / 'VERSION').read_text(encoding='utf-8').strip()
TITLE = f'Noble Archimedes Beta {VERSION}'
SDK_PIN = '91146ff6e2a218b19c1109dc665d06020472ab57'
DONOR_PIN = '1d9edf6b2bdbb683cb95dff5b317916aec31db84'
REDIST_FILES = ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
BRANDING = ('launcher-background.png', 'launcher-theme.wav',
            'noble-archimedes.ico', 'noble-icon.png', 'PROVENANCE.md')
ROOT_REQUIRED = ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'docs/LICENSING.md',
                 'licenses/GPL-2.0.txt', 'licenses/GPL-3.0.md',
                 'licenses/CC0-1.0.txt', 'licenses/XboxXBE-Recomp-MIT.txt',
                 'xbox/third_party/castlevania-cod-pc/LICENSE',
                 'xbox/third_party/castlevania-cod-pc/README.md',
                 f'release/windows/README-{VERSION}.txt') + tuple('assets/noble/' + n for n in BRANDING)
SDK_REQUIRED = ('LICENSE', 'THIRD_PARTY.txt')
BUILD_REQUIRED = ('NobleArchimedes.exe', 'NobleArchimedesLauncher.exe',
                  'compiler/clang.exe', 'compiler/lld-link.exe',
                  'compiler/xbe_recomp.exe', 'compiler/LLVM-LICENSE.txt',
                  'compiler/NobleArchimedes.lib', 'compiler/config/game.toml',
                  'compiler/include/cpu/x86_context.h', 'compiler/include/cpu/x86_ops.h',
                  'compiler/include/freestanding/cmath',
                  'compiler/include/freestanding/cstdint',
                  'compiler/include/freestanding/cstring',
                  'compiler/include/freestanding/limits',
                  'compiler/include/freestanding/type_traits',
                  'compiler/xdk/4831/layout.toml', 'compiler/xdk/5849/layout.toml',
                  'version.txt')


def read_required(path):
    """Refuse redirects and empty files before any output directory is created."""
    if any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError(f'symlink input forbidden: {path}')
    if not path.is_file() or not path.stat().st_size:
        raise ValueError(f'missing or empty required file: {path}')
    return path.read_bytes()


def find_redist(override=None):
    if override:
        candidate = Path(override).absolute()
        for name in REDIST_FILES:
            read_required(candidate / name)
        return candidate
    program_files = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
    vswhere = program_files / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installations = []
    if vswhere.is_file():
        result = subprocess.run([str(vswhere), '-all', '-products', '*',
                                 '-property', 'installationPath'],
                                capture_output=True, text=True, check=True)
        installations.extend(Path(line) for line in result.stdout.splitlines() if line.strip())
    for base in (program_files, Path(os.environ.get('ProgramFiles', r'C:\Program Files'))):
        installations.extend((base / 'Microsoft Visual Studio').glob('*/*'))
    candidates = []
    for install in installations:
        candidates.extend((install / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC*.CRT'))
    for candidate in sorted(set(candidates), reverse=True):
        if all((candidate / n).is_file() for n in REDIST_FILES):
            return candidate
    raise ValueError('VS x64 C++ redistributables missing; supply --redist CRT_DIRECTORY')


def notice_file(path):
    return bool(re.match(r'^(license|licence|copying|copyright|notice|third[_-]?party)([._-].*|$)',
                         path.name, re.IGNORECASE))


def validate_payload(payload):
    for name in payload:
        path = Path(name)
        lower = name.lower()
        if path.is_absolute() or '..' in path.parts:
            raise ValueError(f'unsafe archive path: {name}')
        if any(part in ('work', 'generated', 'captures', 'saves') for part in path.parts):
            raise ValueError(f'forbidden directory: {name}')
        if path.suffix.lower() in ('.xbe', '.iso', '.pdb', '.cpp', '.obj'):
            raise ValueError(f'forbidden runtime file: {name}')
        if path.suffix.lower() == '.dll' and path.name not in REDIST_FILES:
            raise ValueError(f'game or unknown DLL forbidden: {name}')
        if path.suffix.lower() == '.lib' and path.name not in ('NobleArchimedes.lib', 'NobleArchimedesEU.lib'):
            raise ValueError(f'unknown import library: {name}')
        if lower.startswith('compiler/work/'):
            raise ValueError(f'compiler work forbidden: {name}')


def package(root, build, sdk, redist):
    root, build, sdk, redist = (Path(p).absolute() for p in (root, build, sdk, redist))
    payload = {}
    for name in ROOT_REQUIRED:
        data = read_required(root / name)
        if name.startswith('assets/noble/'):
            dest = 'branding/' + Path(name).name
        elif name.startswith('release/windows/'):
            dest = 'README.txt'
        else:
            dest = 'licenses/project/' + name
        payload[dest] = data
    for name in BUILD_REQUIRED:
        payload[name] = read_required(build / name)
    if payload['version.txt'].decode().strip() != VERSION:
        raise ValueError(f'build version must be {VERSION}')
    for name in SDK_REQUIRED:
        payload['licenses/sdk/' + name] = read_required(sdk / name)
    for name in REDIST_FILES:
        data = read_required(redist / name)
        payload[name] = payload['compiler/' + name] = data
    # Actual fetched source trees include nested/transitive license notices.
    # Only license text names are considered; build outputs are never scanned.
    sources = sorted((build / '_deps').glob('*-src'))
    if not sources:
        raise ValueError('actual dependency source licenses unavailable: build/_deps/*-src')
    for source in sources:
        notices = [p for p in source.rglob('*') if p.is_file() and notice_file(p)
                   and '.git' not in p.parts and p.suffix.lower() not in ('.c', '.h', '.cpp', '.cmake', '.py')]
        if not notices:
            raise ValueError(f'no dependency license found: {source.name}')
        for notice in notices:
            dest = 'licenses/dependencies/' + source.name + '/' + notice.relative_to(source).as_posix()
            payload[dest] = read_required(notice)
    regions = ['USA']
    # A matching EU host/library pair can be shipped, but existence is not testing.
    if (build / 'NobleArchimedesEU.exe').is_file() and (build / 'compiler/NobleArchimedesEU.lib').is_file():
        for name in ('NobleArchimedesEU.exe', 'compiler/NobleArchimedesEU.lib', 'compiler/config/game_eu.toml'):
            payload[name] = read_required(build / name)
        regions.append('EU')
    validate_payload(payload)
    revision = subprocess.run(['git', '-C', str(root), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    manifest = {'title': TITLE, 'version': VERSION, 'regions': regions,
                'project_commit': revision.stdout.strip() if revision.returncode == 0 else None,
                'eu_qualified': False, 'sdk_pin': SDK_PIN, 'donor_pin': DONOR_PIN,
                'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())}}
    payload['manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    # Exclusively create a new directory; preserve all previous artifacts even on failure.
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = root / 'output/releases' / f'v{VERSION}-{stamp}-{uuid.uuid4().hex[:8]}'
    output.mkdir(parents=True, exist_ok=False)
    archive = output / f'Noble-Archimedes-{VERSION}-Windows-x64.zip'
    partial = archive.with_suffix('.zip.partial')
    with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name, data in sorted(payload.items()):
            z.writestr(name, data)
        for directory in ('assets/', 'textures/'):
            z.writestr(directory, b'')
    with zipfile.ZipFile(partial) as z:
        if z.testzip() is not None:
            raise ValueError('ZIP CRC validation failed')
        for name, digest in manifest['sha256'].items():
            if hashlib.sha256(z.read(name)).hexdigest() != digest:
                raise ValueError(f'ZIP SHA256 validation failed: {name}')
        validate_payload({n: None for n in z.namelist() if not n.endswith('/')})
    partial.rename(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n', encoding='utf-8')
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build', type=Path)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--redist', type=Path, help='VS x64 Microsoft.VC*.CRT directory')
    args = parser.parse_args()
    root = args.root.absolute()
    build = args.build or root / 'output/jmafort-port/out/build/release'
    sdk = args.sdk or root / 'output/jmafort-sdk'
    print(package(root, build, sdk, find_redist(args.redist)))


if __name__ == '__main__':
    main()
