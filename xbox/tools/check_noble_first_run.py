"""Exercise BYOG startup in a new sandbox without a prebuilt game DLL.

Uses the imported scenario protocol, an isolated save/settings folder and a
Windows-only PATH. This is a local first-run check, not clean-Windows package
qualification: the machine's installed runtime DLLs remain available.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import time

import jmafort_backend as backend


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=backend.WORK / 'out/build/release')
    parser.add_argument('--assets', type=Path, default=backend.WORK / 'assets')
    args = parser.parse_args()
    build, assets = args.build.resolve(), args.assets.resolve()
    for name in ('NobleArchimedes.exe', 'NobleArchimedesLauncher.exe', 'compiler', 'branding'):
        if not (build / name).exists():
            parser.error(f'Missing build input: {build / name}')
    if not assets.is_dir():
        parser.error(f'Missing local game assets: {assets}')
    sandbox = backend.ROOT / 'output' / ('noble-first-run-' + time.strftime('%Y%m%d-%H%M%S'))
    sandbox.mkdir()
    for name in ('NobleArchimedes.exe', 'NobleArchimedesLauncher.exe'):
        shutil.copy2(build / name, sandbox / name)
    backend.link_directory(build / 'compiler', sandbox / 'compiler')
    backend.link_directory(assets, sandbox / 'assets')
    shutil.copytree(build / 'branding', sandbox / 'branding')
    assert not (sandbox / 'NobleArchimedes.dll').exists()
    spec = importlib.util.spec_from_file_location('noble_cvtest', backend.WORK / 'tools/cvtest/cvtest.py')
    cvtest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cvtest)
    allowed = ('ALLUSERSPROFILE', 'APPDATA', 'CommonProgramFiles', 'CommonProgramFiles(x86)',
               'CommonProgramW6432', 'COMPUTERNAME', 'ComSpec', 'HOMEDRIVE', 'HOMEPATH',
               'LOCALAPPDATA', 'LOGONSERVER', 'NUMBER_OF_PROCESSORS', 'OS', 'PATHEXT',
               'PROCESSOR_ARCHITECTURE', 'PROCESSOR_IDENTIFIER', 'PROCESSOR_LEVEL',
               'PROCESSOR_REVISION', 'ProgramData', 'ProgramFiles', 'ProgramFiles(x86)',
               'ProgramW6432', 'PUBLIC', 'SystemDrive', 'SystemRoot', 'TEMP', 'TMP',
               'USERDOMAIN', 'USERNAME', 'USERPROFILE', 'windir')
    env = {key: os.environ[key] for key in allowed if key in os.environ}
    windows = Path(os.environ.get('SystemRoot', r'C:\Windows'))
    env['PATH'] = ';'.join(map(str, [windows / 'System32', windows, windows / 'System32/Wbem']))
    print(f'First-run sandbox: {sandbox}', flush=True)
    passed = cvtest.run(str(backend.WORK / 'tools/cvtest/scenarios/newgame.json'), None, False,
                       install=sandbox, env=env)
    dll = sandbox / 'NobleArchimedes.dll'
    if not dll.is_file():
        raise RuntimeError('First-run compiler did not produce the DLL')
    print(f'First-run DLL size: {dll.stat().st_size}', flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
