# Contributing to Noble Archimedes

**Please fork, experiment and send pull requests.** The primary target is the Xbox D3D12 BYOG port based on jmafort/CastlevaniaCOD-PC; the separate PS2 host remains experimental. We want reproducible builds, correct behavior and sustained gameplay performance. Compiler/runtime knowledge helps, but documentation improvements, clean-build reports and small synthetic regression tests are valuable contributions too.

Read the [build guide](BUILDING.md), [license scope](docs/LICENSING.md) and [upstream import record](docs/CASTLEVANIA_COD_PC_IMPORT.md). The Xbox donor snapshot must remain pristine; maintained integration and branding changes belong in project helpers/overlays and their verification checks. For experimental PS2 work, see [research status](docs/PROJECT_STATUS.md) and [configuration recipes](docs/BUILD_VARIANTS.md). Private optimized VU/game-derived modules are not contribution prerequisites.

## Fork and start a focused change

Use GitHub's **Fork** button or the [fork page](https://github.com/sergiomanzur/noble-archimedes/fork). Replace `YOUR_GITHUB_NAME` with your account:

```powershell
git clone --recurse-submodules https://github.com/YOUR_GITHUB_NAME/noble-archimedes.git
cd noble-archimedes
git remote add upstream https://github.com/sergiomanzur/noble-archimedes.git
git switch -c codex/describe-your-change
```

Follow the primary Xbox instructions in BUILDING.md. Only PS2 contributors need to apply the PS2Recomp patch, as described there. A docs/helper-only change does not need game assets. Keep changes focused enough to review and explain the concrete problem they solve.

Useful starting points:

- Improve the Xbox launcher, original branding, setup diagnostics and reproducible builds while retaining donor attribution.
- Validate EU input, HD texture packs, save/load, audio, movies and gameplay across more rooms; report exactly what was exercised.

- Make clean Windows setup reproducible: configurable toolchain paths, dependency manifests and clearer missing-input diagnostics.
- Teach the manual helpers about build profiles and the hybrid host definition, with tests for the resulting build/link inputs.
- Improve generic VU state, arithmetic/rounding or pipeline tests using synthetic microprograms you wrote yourself.
- Close renderer/GS boundary regressions with synthetic packet fixtures and retained upstream license notices.
- Develop source-only generators/harnesses for the native engine seam; derive game-specific translations locally.
- Improve controls, accessibility, docs and bug reproduction. State which gameplay behavior was actually checked.

For larger changes, open an [issue](https://github.com/sergiomanzur/noble-archimedes/issues) describing the proposed boundary and how you will verify it. Small fixes and documentation PRs can go straight to review. Contributions to the dependencies themselves are also welcome upstream when appropriate.

## Working on the patched runtime

`tools/PS2Recomp` is a pinned submodule. Most project-specific runtime work is carried by `patches/ps2recomp-progress.patch`; uncommitted submodule edits are not included by committing the parent repository.

After applying the existing patch, regenerate it from the pinned base with an explicit source-path list. This captures edits to the existing runtime patch files, including its new-file sections:

```powershell
@'
from pathlib import Path
import re
import subprocess
runtime = Path("tools/PS2Recomp")
patch = Path("patches/ps2recomp-progress.patch")
base = "75d729ce40d7eed9649fd4bb05628dee520f3d0c"
paths = re.findall(r"^diff --git a/(.*?) b/", patch.read_text(encoding="utf-8"), re.M)
paths = sorted(set(paths))
if not paths:
    raise SystemExit("No runtime source paths found")
# Add any intentional new generic source paths to paths before this loop.
for name in paths:
    known = subprocess.run(["git", "ls-files", "--error-unmatch", "--", name],
                           cwd=runtime, capture_output=True)
    if known.returncode and (runtime / name).is_file():
        subprocess.run(["git", "add", "--intent-to-add", "--", name],
                       cwd=runtime, check=True)
data = subprocess.check_output([
    "git", "diff", "--no-ext-diff", "--full-index", base,
    "--", *paths
], cwd=runtime)
if b"GIT binary patch" in data or b"Binary files " in data:
    raise SystemExit("Binary payload rejected")
patch.write_bytes(data)
'@ | python -
```

The intent-to-add entries make new source files visible in `git diff`; this does not create an upstream runtime commit. Add intentional new generic source paths to the list before running the loop, and update the explicit runtime allowlist in `tools/prepare_publication.py`. Inspect deletions and all generated patch paths. Do not change the pinned base casually. The exporter is a maintainer tool for an assembled development checkout, not a build prerequisite or a general patch-refresh command for every clone.

Before submitting, apply-check and apply the complete patch on a **second clean clone at the pinned upstream base**, then compare the affected source files and run relevant tests there. This catches missing untracked files. Do not commit a local-only submodule hash that others cannot fetch, and do not reset a dirty submodule to make a check pass. If patch packaging is blocking your contribution, explain that in the PR so maintainers can help.

## Verification

Run the helper suites relevant to your change:

```powershell
python -m unittest discover -s tests -p test_build_helpers.py
python -m unittest discover -s tests -p test_guest_entries_generation.py
python -m unittest discover -s tools/tests -p test_optimize_register_functions.py
```

Two integration checks require locally generated game inputs and intentionally skip when those are absent. Keep that distinction in test reports. The real GS module suite is documented in the hybrid build recipe and requires a built module and D3D11 device.

For runtime/renderer changes, include the relevant correctness checks and a gameplay check when you can run it. For performance changes, report before/after measurements with identical configuration, scene/input and stated background load. Include guest-update and fresh-image counters and intervals, module hashes, warm versus cold timing, and any visual/state differences. Presentation/VBlank FPS alone does not demonstrate gameplay speed. Never discard consumed VU state, ordering or completion merely to improve a counter.

If you cannot run a check, say so. A precise limitation is more useful than an unsupported success claim.

## Keep game material and local artifacts out of contributions

Submit code you have the right to contribute. Original Noble Archimedes branding and synthetic audio may be contributed in `assets/noble/` under CC0; identify how they were made and include no game artwork or soundtrack. Preserve upstream notices and applicable licenses. Original contributions use the scoped licenses in [LICENSE](LICENSE); derivative contributions retain compatible upstream terms. The GPLv2 Xbox application and GPLv3 PS2 application remain separate programs. Do not copy incompatible implementation between them. Identify third-party code and its provenance in your PR. See [third-party notices](THIRD_PARTY_NOTICES.md) and [publication scope](docs/PUBLICATION.md).

Do not attach or commit ISO/XBE/ELF/BIOS/IRX files, game assets, translated game implementations or microcode, generated chunks, RAM/VRAM/VU/GIF captures, oracle data, saves, compiled binaries or dependency archives. Share aggregate measurements, generic fixtures you authored and source tools that regenerate required inputs locally. Check staged files, not just `.gitignore`. Keep any game-specific comparison evidence local.

`PUBLICATION_MANIFEST.json` tracks ordinary published files. Refresh hashes for edited files and add intentional new source/docs files; do not add game/build outputs. If you need help refreshing it, mention that in the PR. Maintainers also verify the submodule pin and publication scope.

## Submit your pull request

Review and stage named files rather than the whole development tree:

```powershell
git diff
git add README.md
# Replace/add paths above with the intentional files in your change.
git diff --cached --stat
git diff --cached --check
git commit -m "Describe the concrete improvement"
git push -u origin codex/describe-your-change
```

Open a PR from your fork to this repository's `main`. Explain the problem, resulting behavior, profile/toolchain, checks run and remaining limitations. For an optimization, include measured before/after results and the correctness evidence. Documentation-only PRs can list link/command checks instead of gameplay tests.

To update your fork's local main later, first finish or commit your current work, then:

```powershell
git switch main
git fetch upstream
git merge --ff-only upstream/main
git push origin main
```

Inspect submodule changes before updating a patched runtime checkout; an upstream pin/patch update can require a fresh clone or careful reapplication. Never discard local runtime edits automatically.

Thank you for helping turn a research prototype into a reproducible port. Small, well-explained contributions make it easier for the next person to join.
