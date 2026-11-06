# Third-party source and licensing

The optional legacy Xbox runtime is [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), pinned at `110447d072d319f9bb8fdc4e2f582a111753e284`. Its local generic runtime changes are preserved in `patches/xboxrecomp-local-runtime.patch` under the upstream MIT terms; see `licenses/xboxrecomp-MIT.txt`. Fetch with `python tools/fetch_legacy_xbox_runtime.py`. This runtime is not part of the primary v0.9.0b executable or runtime ZIP.

No game assets, proprietary SDKs, dependency binaries or generated game implementations are included.

| Project | Reference | Treatment |
| --- | --- | --- |
| PS2Recomp | https://github.com/ran-j/PS2Recomp — `75d729ce40d7eed9649fd4bb05628dee520f3d0c` | Submodule plus runtime source patch. GPL-3.0 upstream terms continue to apply. Full GPLv3 text in `licenses/GPL-3.0.md`; the upstream license remains in `tools/PS2Recomp/LICENSE`. |
| paraLLEl-GS / Granite | https://github.com/Arntzen-Software/parallel-gs | Acquired locally by pinned fetch script; small transfer patch included. Retain each upstream/dependency notice. |
| PCSX2 | https://github.com/PCSX2/pcsx2 — `646df006a4688063e56c0826e7ea4b8bb0c112c4` | Acquired locally. GPL-3.0-or-later; see upstream `COPYING.GPLv3` and third-party notices. GS consumer/glue is intended for this GPL-covered combination. |
| raylib, Dear ImGui, rlImGui, FFmpeg and other build dependencies | PS2Recomp build references | Fetched locally, not bundled. Their own license/notice terms apply. |
| Play! | https://github.com/jpd002/Play- | Optional VU loader interface only; optimized DLL and translated game microprograms excluded. |
| CastlevaniaCOD-PC | https://github.com/jmafort/CastlevaniaCOD-PC — `1d9edf6b2bdbb683cb95dff5b317916aec31db84` | Complete source snapshot in `xbox/third_party/castlevania-cod-pc`, including upstream GPL-2.0 `LICENSE` and README credits. GPLv2-only donor source is not relicensed. The SDK patch is an explicit MIT exception, Copyright (c) 2026 TGP482, as stated in the donor README. Foundation of the primary Xbox executable; not linked into the PS2 host. Adapted diagnostic decoder and release metadata retain these terms and credits. See `docs/CASTLEVANIA_COD_PC_IMPORT.md`. |
| Existing Xbox toolkit | `tools/xboxrecomp` | Separate MIT-licensed toolkit, Copyright (c) 2026 sp00nz, with its own license and dependency notices. Do not confuse it with TGP482's donor SDK. |
| REA tooling | `tools/rea` | MIT, Copyright (c) 2026 morluto, with separately licensed bundled tools and dependencies. Preserve its LICENSE, licenses directory and vendor notices. |
| Experimental Xbox OpenGL renderer | `xbox/src/d3d8gl/` | Includes code carried over from a Halo renderer. Project license defaults do not cover borrowed upstream material. Its source/pin/license provenance must be retained or completed before redistribution of this experimental component; no new license is asserted for that material here. |
| Xbox XBE Recomp (TGP482) | https://github.com/TGP482/XboxXBE-Recomp — `91146ff6e2a218b19c1109dc665d06020472ab57` | Fetched locally only for the imported D3D12 backend, with the donor's SDK patch. MIT upstream license and `THIRD_PARTY.txt` remain in that checkout. Distinct from the existing `tools/xboxrecomp` runtime. |

Source references do not replace corresponding-source/license/notice obligations when distributing combined works or binaries. Original project contributions now use the scoped license grant in [LICENSE](LICENSE) and [docs/LICENSING.md](docs/LICENSING.md). That grant does not relicense upstream work. Public hosting does not mean public domain. No rights to Konami game content are granted.

## Primary Xbox attribution and packaging

The Xbox overlay also adapts later CastlevaniaCOD-PC work through upstream commit `7635802e41445486d90b3dbb6b73b5a0fc13392b`. Credit to Jovane Mafort and Calm-Consideration41 (PlatinumMimic) for the view, widescreen, input, sprint, and HD movie work. The original GPLv2 and SDK MIT terms continue to apply to their respective code.

The complete imported README preserves jmafort's credits to [Pazzi7054](https://github.com/Pazzi7054/CastlevaniaCOD-PC), [TGP482](https://github.com/TGP482/XboxXBE-Recomp), [Neguchads](https://github.com/Neguchads/Castlevania-COD-Recomp) and [PCSX2](https://pcsx2.net). Retain those credits, the imported GPLv2 license and the SDK patch's MIT copyright notice in derivative distributions. The source snapshot remains pristine; Noble Archimedes integration and branding are applied to a local working copy.

The imported SDK and build also fetch libraries such as SDL, Dear ImGui, xxHash and toml++. Their own LICENSE/THIRD_PARTY notices and those of transitive components remain authoritative. Packaging must inventory the actual fetched versions and distributed runtime/compiler files. This table is not a substitute for that inventory.

No PS2 texture pack, game logo, game soundtrack or game-derived icon is bundled. Original Noble Archimedes branding assets and synthetic audio in `assets/noble/` have the scoped CC0 dedication in the root license. The donor's tool for extracting a game icon does not confer redistribution rights in that icon; the project build uses its own branding.
