# CastlevaniaCOD-PC import plan

Goal: retain all reusable donor work and make its complete D3D12 host buildable alongside the existing recomp, while adapting useful diagnostics to the current host.

Architecture: preserve a byte-for-byte source snapshot in xbox/third_party/castlevania-cod-pc. Build a private working copy under output/jmafort-port using SDK 91146ff6e2a218b19c1109dc665d06020472ab57 and the donor patch. Neither SDK can replace the other's incompatible register, graphics or hook interfaces. Add a read-only Python state decoder using the donor's release-specific metadata.

Constraints: preserve existing workspace edits; no generated game code/assets in tracked source; retain GPL-2.0 license and upstream attribution; do not claim backend features were ported into the existing renderer. No automatic installation or release publication.

- [x] Preserve all tracked donor files, SHA-256 inventory, commit and SDK provenance.
- [x] Test decoder against synthetic memory including absent, corrupt and nonfinite data, then implement it and its Windows process adapter.
- [x] Add isolated preparation/build helpers; verify SDK revision, patch application, XBE identity, source inventory and repeatable preparation.
- [x] Build donor runtime/launcher and generated game code locally if available tooling permits; record exact results and limitations.
- [x] Audit all donor modules and document current reuse versus remaining integration.
- [x] Run focused tests, inspect the resulting diff, and retain a verification record.

Rulings: preserve all ongoing edits in the current workspace; build in ignored output folders rather than moving active work to a separate checkout. Keep the incompatible donor runtime as a separate optional executable. Normalize CRLF to LF for the source inventory so checkout line-ending settings do not cause false integrity failures. EU builds require a separate eu-play asset root because the upstream runtime only searches root XBE/ISOs.

Review: fresh reviewer identified the EU asset-discovery defect; fixed and covered by a repeatable staging test. Follow-up review reported no remaining P1/P2 findings.

Verification: full USA build exit 0; 13 focused checks and both CTest entries pass; upstream newgame scenario passes all 7 steps with short courtyard samples of 59.5–59.8 FPS. Details and remaining qualification gaps are in docs/CASTLEVANIA_COD_PC_IMPORT.md. No commits or remote publication were made.
