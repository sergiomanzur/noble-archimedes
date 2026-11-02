"""Fetch the optional legacy Xbox research runtime and apply its reviewed MIT patch."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / 'tools/xboxrecomp'
PIN = '110447d072d319f9bb8fdc4e2f582a111753e284'


def main():
    if not TARGET.exists():
        subprocess.run(['git', 'clone', 'https://github.com/sp00nznet/xboxrecomp.git', str(TARGET)], check=True)
        subprocess.run(['git', '-C', str(TARGET), 'checkout', '--detach', PIN], check=True)
    revision = subprocess.check_output(['git', '-C', str(TARGET), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise RuntimeError(f'Legacy runtime must be at {PIN}; found {revision}')
    patch = ROOT / 'patches/xboxrecomp-local-runtime.patch'
    reverse = subprocess.run(['git', '-C', str(TARGET), 'apply', '--reverse', '--check', str(patch)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if reverse.returncode:
        subprocess.run(['git', '-C', str(TARGET), 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', str(TARGET), 'apply', str(patch)], check=True)
    print(f'Optional legacy runtime prepared at {PIN}')


if __name__ == '__main__':
    main()
