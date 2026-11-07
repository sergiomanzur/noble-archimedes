"""Install a runtime ZIP outside the source tree and exercise first-run BYOG.

The test checks every PE dependency against packaged files or Windows system
components, limits PATH to Windows, and uses separate test settings/saves.
The local game input and generated game DLL never enter the release ZIP.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'output/jmafort-port'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--game', type=Path, default=ROOT / 'extracted')
    parser.add_argument('--advanced', action='store_true')
    args = parser.parse_args()
    work = ROOT / 'output/noble-next/port' if args.advanced else WORK
    import sys
    sys.path.insert(0, str(work / 'tools/cvtest'))
    import cvtest
    install = load('noble_install', work / 'tools/cvtest/install.py')
    package = load('noble_package', ROOT / 'tools/release_noble.py')
    folder = Path(tempfile.gettempdir()).resolve() / ('NobleArchimedes-release-test-' + time.strftime('%Y%m%d-%H%M%S'))
    for parent in folder.parents:
        if (parent / 'assets').is_dir():
            raise RuntimeError(f'Unrelated assets directory would affect the test: {parent / "assets"}')
    with zipfile.ZipFile(args.archive) as archive:
        package.validate_payload({name: None for name in archive.namelist() if not name.endswith('/')})
        archive.extractall(folder)
    errors = []
    for path in folder.rglob('*'):
        if path.suffix.lower() not in ('.exe', '.dll'):
            continue
        for dependency in install.imports(path):
            lower = dependency.lower()
            if lower in ('noblearchimedes.dll', 'noblearchimedeseu.dll'):
                continue
            if lower.startswith(('api-ms-win-', 'ext-ms-')) or (path.parent / dependency).is_file():
                continue
            if (install.SYSTEM32 / dependency).is_file() and not lower.startswith(install.REDISTRIBUTED):
                continue
            errors.append(f'{path.relative_to(folder)} is missing {dependency}')
    if errors:
        raise RuntimeError('\n'.join(errors))
    print(f'Package dependency checks passed; installed {folder}', flush=True)
    install.put_game(args.game.resolve(), folder / 'assets')
    env = {key: os.environ[key] for key in install.WINDOWS_VARS if key in os.environ}
    windows = Path(os.environ.get('SystemRoot', r'C:\Windows'))
    env['PATH'] = ';'.join(map(str, [install.SYSTEM32, windows, install.SYSTEM32 / 'Wbem']))
    checks = [{'step': 'package', 'label': 'runtime ZIP dependencies and no prebuilt game DLL',
               'ok': True, 'errors': [], 'warnings': []}]
    passed = cvtest.run(str(work / 'tools/cvtest/scenarios/newgame.json'), None, False,
                       install=folder, env=env, checks=checks)
    print(f'Test settings and saves retained at {folder}', flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
