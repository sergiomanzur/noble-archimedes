"""Apply only the tracked optional-input-provider change, idempotently."""
from pathlib import Path
import subprocess
import sys

def ensure_provider(runtime: Path, patch: Path) -> bool:
    runtime, patch = Path(runtime).resolve(), Path(patch).resolve()
    if not patch.is_file():
        raise RuntimeError(f"Input provider patch missing: {patch}")
    def git(*args):
        return subprocess.run(["git", "-C", str(runtime), "apply", *args, str(patch)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if git("--reverse", "--check").returncode == 0:
        return False
    check = git("--check")
    if check.returncode:
        raise RuntimeError("Input provider patch does not match this runtime; local edits were preserved.\n"
                           + check.stderr.decode(errors="replace"))
    result = git()
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return True

if __name__ == "__main__":
    root = Path(__file__).resolve().parents[2]
    runtime = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "tools/xboxrecomp"
    try:
        changed = ensure_provider(runtime, root / "patches/xbox-modern-input-provider.patch")
    except RuntimeError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
    print("Input provider patch applied" if changed else "Input provider patch already present")
