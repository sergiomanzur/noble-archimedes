"""Prepare/build the imported D3D12 port in an isolated local output folder."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT = ROOT / 'xbox/third_party/castlevania-cod-pc'
WORK = ROOT / 'output/jmafort-port'
SDK = ROOT / 'output/jmafort-sdk'
OVERLAY = ROOT / 'xbox/noble/overlay'
BRANDING = ROOT / 'assets/noble'
RELEASE_VERSION = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
RELEASES = {
    '423bc19b7ced263570dce5e7cd5d81954dfbe13c': 'usa',
    'b1910aaa4897cc9ca9627190289bf18d6a0943bd': 'eu',
}


def run(*args, cwd=None):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def verify_snapshot(root=SNAPSHOT):
    manifest = json.loads((root / 'IMPORT.json').read_text(encoding='utf-8'))
    for name, expected in manifest['files'].items():
        path = root / name
        data = path.read_bytes() if path.is_file() else None
        if data is not None and manifest.get('hash_normalization') == 'CRLF to LF':
            data = data.replace(b'\r\n', b'\n')
        if data is None or hashlib.sha256(data).hexdigest() != expected:
            raise RuntimeError(f'Imported source differs from pinned snapshot: {name}')
    return manifest


def identify_xbe(path):
    digest = hashlib.sha1(path.read_bytes()).hexdigest()
    if digest not in RELEASES:
        raise RuntimeError(f'Unsupported XBE SHA-1 {digest}: expected an unmodified USA or EU Xbox release')
    return RELEASES[digest]


def branded_source(name, data):
    """Apply the small reviewed product-name adaptation to pristine donor text."""
    if name not in ('CMakeLists.txt', 'config/game.toml', 'config/game_eu.toml',
                    'src/common/game_info.cpp', 'tools/cvtest/cvtest.py',
                    'tools/cvtest/install.py'):
        return data
    text = data.decode('utf-8').replace('\r\n', '\n')
    if name == 'src/common/game_info.cpp':
        text = text.replace('.title = L"Castlevania: Curse of Darkness"', '.title = L"Noble Archimedes"')
        text = text.replace('.short_title = L"Castlevania"', '.short_title = L"Noble Archimedes"')
        text = text.replace('CV_PICK("Castlevania", "CastlevaniaEU")',
                            'CV_PICK("NobleArchimedes", "NobleArchimedesEU")')
        # Keep internal class/log/settings names stable for the existing diagnostics.
    else:
        # Whole identifiers keep the recompiler config, import library, runtime
        # release table and DLL loader names in agreement, including first start.
        names = {'Castlevania': 'NobleArchimedes', 'CastlevaniaEU': 'NobleArchimedesEU',
                 'CastlevaniaLauncher': 'NobleArchimedesLauncher'}
        text = re.sub(r'\b(?:CastlevaniaLauncher|CastlevaniaEU|Castlevania)\b',
                      lambda match: names[match.group()], text)
    if name == 'CMakeLists.txt':
        numeric_version = re.match(r'\d+\.\d+\.\d+', RELEASE_VERSION).group()
        text = text.replace('project(castlevania VERSION 1.5.1', f'project(noble_archimedes VERSION {numeric_version}')
        text = text.replace('@PROJECT_VERSION@', RELEASE_VERSION)
        text = text.replace('${PROJECT_VERSION}\\n', RELEASE_VERSION + '\\n')
        text = text.replace('option(CV_GAME_ICON ', 'set(CV_GAME_ICON OFF CACHE BOOL "No extracted game icon" FORCE)\noption(CV_GAME_ICON ')
        text = text.replace('NobleArchimedes: Curse of Darkness, PC port', 'Noble Archimedes - Xbox recompilation')
        text = text.replace('"CastlevaniaCOD-PC"', '"Noble Archimedes"')
        text = text.replace('"CastlevaniaCOD-PC, a fan project"', '"Noble Archimedes contributors"')
        text = text.replace('NobleArchimedes is a trademark of Konami.', 'Castlevania is a trademark of Konami.')
        text += '\n# Noble Archimedes product resources; donor snapshot stays pristine.\ninclude(${CMAKE_CURRENT_SOURCE_DIR}/noble-branding.cmake)\n'
    return text.encode('utf-8')


def sync_sources(manifest, snapshot=SNAPSHOT, work=WORK, overlay=OVERLAY):
    """Update our generated working source; reject edits outside our last output."""
    record = work / '.noble-source-state.json'
    previous = json.loads(record.read_text()) if record.exists() else {}
    desired = {name: branded_source(name, (snapshot / name).read_bytes()) for name in manifest['files']}
    if overlay.exists():
        for path in overlay.rglob('*'):
            if path.is_file():
                desired[path.relative_to(overlay).as_posix()] = path.read_bytes()
    def digest(data):
        return hashlib.sha256(data.replace(b'\r\n', b'\n')).hexdigest()
    # Preflight all files before writing any of them.
    for name, data in desired.items():
        target = work / name
        if not target.exists():
            continue
        current = digest(target.read_bytes())
        original = snapshot / name
        allowed = {digest(data), previous.get(name)}
        if original.is_file():
            allowed.add(digest(original.read_bytes()))
        if current not in allowed:
            raise RuntimeError(f'Local working source was edited; preserve it before updating: {target}')
    for name, data in desired.items():
        target = work / name
        if not target.exists() or digest(target.read_bytes()) != digest(data):
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
    record.write_text(json.dumps({name: digest(data) for name, data in desired.items()}, indent=2) + '\n')


def link_directory(source, target):
    if target.exists():
        if target.resolve() != source.resolve():
            raise RuntimeError(f'Refusing to replace existing directory: {target}')
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt':
        run('cmd', '/c', 'mklink', '/J', target, source)
    else:
        target.symlink_to(source, target_is_directory=True)


def stage_eu(build, assets):
    """Place EU beside its own assets so runtime discovery cannot select USA."""
    play = build / 'eu-play'
    play.mkdir(parents=True, exist_ok=True)
    link_directory(assets, play / 'assets')
    link_directory(build / 'compiler', play / 'compiler')
    if (build / 'branding').is_dir():
        shutil.copytree(build / 'branding', play / 'branding', dirs_exist_ok=True)
    for source in build.iterdir():
        if source.is_file() and source.suffix.lower() in ('.exe', '.dll'):
            shutil.copy2(source, play / source.name)
    return play


def prepare(assets, eu_assets=None):
    manifest = verify_snapshot()
    if identify_xbe(assets / 'default.xbe') != 'usa':
        raise RuntimeError('The main build needs the USA XBE; use --eu-assets for the EU release')
    for name in ('data', 'movie', 'sound'):
        if not (assets / name).is_dir():
            raise RuntimeError(f'Missing game directory: {assets / name}')
    if eu_assets and identify_xbe(eu_assets / 'default.xbe') != 'eu':
        raise RuntimeError('--eu-assets must contain the EU XBE')
    sync_sources(manifest)
    if not SDK.exists():
        run('git', 'clone', '--filter=blob:none', '--no-checkout', manifest['sdk_repository'], SDK)
        run('git', '-C', SDK, 'checkout', '--detach', manifest['sdk_commit'])
    revision = subprocess.check_output(['git', '-C', str(SDK), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != manifest['sdk_commit']:
        raise RuntimeError(f'SDK revision {revision} differs from pinned {manifest["sdk_commit"]}')
    patch = SNAPSHOT / 'sdk-patches/xboxxbe-recomp.patch'
    # Check both directions: a second preparation must not apply a patch twice.
    reverse = subprocess.run(['git', '-C', str(SDK), 'apply', '--reverse', '--check', str(patch)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if reverse.returncode:
        run('git', '-C', SDK, 'apply', '--check', patch)
        run('git', '-C', SDK, 'apply', patch)
    # XBE copied rather than linked: preparation never changes the user's copy.
    for source, target in [(assets, WORK / 'assets')] + ([(eu_assets, WORK / 'assets/eu')] if eu_assets else []):
        target.mkdir(parents=True, exist_ok=True)
        xbe = target / 'default.xbe'
        data = (source / 'default.xbe').read_bytes()
        if xbe.exists() and xbe.read_bytes() != data:
            raise RuntimeError(f'Refusing to replace another XBE: {xbe}')
        if not xbe.exists():
            xbe.write_bytes(data)
        for name in ('data', 'movie', 'sound'):
            link_directory(source / name, target / name)
    print(f'Prepared {WORK}; source {manifest["commit"]}; SDK {revision}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=ROOT / 'extracted')
    parser.add_argument('--eu-assets', type=Path)
    parser.add_argument('--verify', action='store_true', help='verify imported source without preparing or downloading')
    parser.add_argument('--build', action='store_true', help='configure and build runtime, launcher and recompiler')
    parser.add_argument('--full-game', action='store_true', help='also compile the translated game DLL (can be slow)')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    try:
        if args.verify:
            manifest = verify_snapshot()
            print(f'Verified {len(manifest["files"])} imported files at {manifest["commit"]}')
            return
        if args.jobs < 1:
            parser.error('--jobs must be positive')
        prepare(args.assets.resolve(), args.eu_assets.resolve() if args.eu_assets else None)
        if args.build:
            build = WORK / 'out/build/release'
            run('cmake', '--preset', 'release', f'-DFETCHCONTENT_SOURCE_DIR_XBE_SDK={SDK.as_posix()}',
                f'-DNOBLE_BRANDING_ASSETS={BRANDING.as_posix()}', '-DCV_GAME_ICON=OFF', cwd=WORK)
            targets = ['NobleArchimedes', 'NobleArchimedesLauncher']
            if args.eu_assets:
                targets.append('NobleArchimedesEU')
            if args.full_game:
                targets += ['NobleArchimedes_code'] + (['NobleArchimedesEU_code'] if args.eu_assets else [])
            run('cmake', '--build', build, '--parallel', args.jobs, '--target', *targets)
            if args.eu_assets:
                stage_eu(build, WORK / 'assets/eu')
            print(f'Built {build}; launch with Launch Noble Archimedes.cmd', flush=True)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
