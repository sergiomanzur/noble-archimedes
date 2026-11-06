"""Archive a release commit plus the exact open-source SDK/dependency inputs."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--commit', default='HEAD')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', args.commit], text=True).strip()
    source = subprocess.check_output(['git', '-C', str(ROOT), 'archive', '--format=tar', commit])
    payload = {}
    with tarfile.open(fileobj=io.BytesIO(source)) as archive:
        for member in archive:
            if member.isfile():
                name = member.name
                if '/generated/' in name or '/recomp/gen/' in name or Path(name).suffix.lower() in ('.xbe', '.iso', '.elf', '.dmp'):
                    raise RuntimeError(f'Private game input/output unexpectedly tracked: {name}')
                payload['project/' + name] = archive.extractfile(member).read()
    sdk = ROOT / 'output/jmafort-sdk'
    sdk_files = subprocess.check_output(['git', '-C', str(sdk), 'ls-files', '-z']).decode().split('\0')
    for name in filter(None, sdk_files):
        path = sdk / name
        if path.is_file() and not path.is_symlink():
            payload['dependencies/xbox-xbe-recomp/' + name] = path.read_bytes()
    dependencies = ROOT / 'output/jmafort-port/out/build/release/_deps'
    pins = {}
    for folder in sorted(dependencies.glob('*-src')):
        revision = subprocess.run(['git', '-C', str(folder), 'rev-parse', 'HEAD'], capture_output=True, text=True)
        pins[folder.name] = revision.stdout.strip() if revision.returncode == 0 else 'source snapshot'
        for path in folder.rglob('*'):
            relative = path.relative_to(folder)
            if not path.is_file() or path.is_symlink() or '.git' in relative.parts:
                continue
            if path.name == '.git':
                continue
            payload['dependencies/' + folder.name + '/' + relative.as_posix()] = path.read_bytes()
    manifest = {'project_commit': commit, 'sdk_commit': '91146ff6e2a218b19c1109dc665d06020472ab57',
                'sdk_patch': 'project/xbox/third_party/castlevania-cod-pc/sdk-patches/xboxxbe-recomp.patch',
                'sdk_patch_applied_to_snapshot': True, 'dependency_pins': pins,
                'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(payload.items())}}
    payload['SOURCE-MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    payload['README-SOURCE.txt'] = (f'Noble Archimedes 0.9.2b corresponding source\nProject commit: {commit}\n\n'
        'project/ is the exact release commit. Read project/BUILDING.md and project/LICENSE.\n'
        'dependencies/xbox-xbe-recomp/ is the actual patched SDK source used to build the release.\n'
        'The donor SDK patch is already applied there: do not apply it twice. Other dependency\n'
        'source trees and their notices are included. The build helper normally fetches pinned\n'
        'sources itself; these snapshots make the compiled runtime inputs available offline.\n'
        'LLVM compiler binaries are a separate Apache-licensed build tool; their license is\n'
        'included with the runtime and upstream source is at https://github.com/llvm/llvm-project.\n'
        'No commercial game inputs or translated game implementation are included.\n').encode()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'x', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data in sorted(payload.items()):
            archive.writestr(name, data)
    with zipfile.ZipFile(args.output) as archive:
        assert archive.testzip() is None
        for name, expected in manifest['sha256'].items():
            assert hashlib.sha256(archive.read(name)).hexdigest() == expected
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest()
    args.output.with_suffix('.zip.sha256').write_text(f'{digest}  {args.output.name}\n')
    print(f'{args.output}: {len(payload)} source files; SHA256 {digest}')


if __name__ == '__main__':
    main()
