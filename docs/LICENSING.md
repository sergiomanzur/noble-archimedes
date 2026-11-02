# Licensing and forks

Forks and redistribution are welcome under the licenses in [LICENSE](../LICENSE). The project is an aggregation of independently licensed components; there is no claim that one license covers every file.

| Material | Applicable terms |
| --- | --- |
| Original project source, generic helpers, tests and documentation, unless excepted below | `GPL-2.0-only OR GPL-3.0-or-later`, recipient's choice |
| Imported Xbox port and GPLv2 adaptations, including `xbox/tools/imported_game_state.py` and its release profiles | `GPL-2.0-only`; retain donor copyright, README credits and LICENSE |
| Donor `sdk-patches/xboxxbe-recomp.patch` | MIT, explicitly stated by the donor; Copyright (c) 2026 TGP482 |
| PS2 host `src/`, PS2 runtime patches and project contributions derived from PS2Recomp/PCSX2 | Project contributions `GPL-3.0-or-later`; retain upstream notices and terms |
| Original art/icons/synthetic audio in `assets/noble/` | `CC0-1.0`, only to the extent the contributor owns rights; code generators use the source default unless marked |
| Dependencies, submodules, copied/adapted third-party material | Their own licenses; a project default cannot override upstream terms |
| Game inputs, generated game implementation, replacement packs and other game material | No rights granted by this repository |

The original-source dual grant lets a fork choose GPLv2 for code used in the Xbox program, or GPLv3 or later for code used in the PS2 program. It grants that choice only for original project contributions. It cannot grant a GPLv3 option for someone else's GPLv2-only source.

## Why there are separate programs

The primary Xbox application is based on [jmafort/CastlevaniaCOD-PC](https://github.com/jmafort/CastlevaniaCOD-PC/tree/1d9edf6b2bdbb683cb95dff5b317916aec31db84). The donor's README specifies GPLv2 for its source and MIT for its SDK patch. We conservatively retain `GPL-2.0-only` for the donor source; the sample “or later” language at the end of the standard GPL text is not treated as an extra grant from the donor.

The experimental PS2 application uses PS2Recomp and optionally PCSX2 under their GPLv3 terms. The Xbox build and PS2 build remain separate executables with separate runtime dependencies. No donor source is linked into the PS2 host. Newly authored shared helpers can select the compatible branch of their dual grant.

GNU's [GPLv3 guide](https://www.gnu.org/licenses/quick-guide-gplv3.html) explains that GPLv2 permits upgrading to GPLv3 when the copyright holder allows later versions. Its [compatibility discussion](https://www.gnu.org/licenses/rms-why-gplv3.en.html) distinguishes combining incompatible code in one program from distributing separate programs. These references explain the project boundary; they do not replace the actual license terms or establish that every future combination is compatible.

Do not copy GPLv2-only Xbox implementation into the GPLv3 PS2 host, or GPLv3-derived implementation into the GPLv2 Xbox host. Independently implement behavior from appropriate research, or obtain a compatible license from the relevant copyright holders.

## Redistributing a fork or build

Keep copyright notices, the applicable full license texts, source attribution and change notices. When distributing GPL-covered executables, meet the corresponding-source and other obligations of the applicable GPL version, including source and build material needed for your covered modifications. A link to an upstream homepage alone does not replace those obligations. Include all runtime/dependency notices required by the actual files you distribute; review fetched dependencies and their transitive components before packaging them.

The repository includes full GPLv2, GPLv3, CC0 and [TGP482 SDK/patch MIT](../licenses/XboxXBE-Recomp-MIT.txt) texts in [licenses](../licenses/). Upstream licenses stay alongside vendored source or in their dependency checkouts. See the component inventory in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Original branding and game content

The CC0 dedication applies solely to the project's original Noble Archimedes art, icon and synthetic audio assets in `assets/noble/`, to the extent rights are held. [CC0's legal text](https://creativecommons.org/publicdomain/zero/1.0/legalcode.en) includes limitations, a fallback license and no trademark or patent grant. Descriptions or prompts are not evidence of ownership in unrelated material. Preserve any specific provenance notice accompanying an asset.

No commercial game logo, soundtrack, texture or character asset is covered by this dedication. A PS2 HD texture pack must be acquired separately and used under its own terms; support for loading a pack grants no right to distribute it. BYOG means users supply their own game inputs, and it does not grant rights to game-derived generated output. Castlevania belongs to its respective rights holders. Noble Archimedes is an independent fan project and claims no endorsement.

## Contributions

Submit material you have the right to license. Original contributions use the scoped project licenses above; derivative contributions retain the compatible upstream license. Identify copied material and its provenance. There is no copyright assignment requirement. A contribution cannot broaden the license of an upstream work merely by changing its header.
