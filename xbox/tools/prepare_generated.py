"""Replay host integration repairs after regenerating the Xbox C sources.

Generated sources are local inputs. Only the small, repeatable host repairs
belong in version control. Running this script twice leaves files unchanged.
"""
from pathlib import Path
import argparse
import re


# Audited from translated guest instructions. Cleanup includes the return VA.
D3D8_ENTRIES = {
    "003BB4C0": 28, "003B6520": 8, "003B66D0": 28,
    "003B6FB0": 12, "003B6B80": 12, "003B75F0": 8,
    "003B7820": 12, "003B79D0": 12, "003B7A60": 12,
    "003B86E0": 20, "003B8820": 4, "003B8880": 4,
    "003B8930": 8, "003B8A50": 16, "003B8E30": 8,
    "003BA740": 20, "003BA8B0": 24, "003BA9F0": 16,
    "003BAE20": 8,
    "003B8AE0": 12, "003B8B40": 12,
    "003BAD40": 8, "003BAD80": 4,
    "003BAC50": 16, "003BAC90": 24, "003BACF0": 12,
}


def repair_d3d8(source: str) -> str:
    # Replace old, guessed hooks and make replay idempotent. Keep the guest
    # implementations in place; the bridge decides which calls it can handle.
    source = re.sub(r"^    if \(noble_d3d8_guest_call\(0x[0-9A-Fa-f]+u, \d+u\)\) return;\n",
                    "", source, flags=re.MULTILINE)
    used = False
    for address, cleanup in D3D8_ENTRIES.items():
        entry = f"void sub_{address}(void)\n{{\n"
        if entry in source:
            source = source.replace(entry, entry +
                f"    if (noble_d3d8_guest_call(0x{address}u, {cleanup}u)) return;\n", 1)
            used = True
    declaration = "extern int noble_d3d8_guest_call(uint32_t va, uint32_t cleanup);"
    if used and declaration not in source:
        source = source.replace('#include "recomp_funcs.h"',
                                '#include "recomp_funcs.h"\n' + declaration, 1)
    return source


def repair_xmv(source: str) -> str:
    # 0x400211 is the XRGB path. Movies lock LIN_A8R8G8B8 (0x12), which
    # selects 0x4005EC instead. Preserve the unused converter's guest alpha.
    source = source.replace("void sub_00400211(void)\n{\n    if (noble_yuv_hook()) return;",
                            "void sub_00400211(void)\n{")
    for name, hook in (("sub_004005EC", "noble_yuv_hook"),
                       ("sub_003B7B20", "noble_field_status_hook")):
        entry = f"void {name}(void)\n{{\n"
        guard = f"    if ({hook}()) return;\n"
        if entry in source and entry + guard not in source:
            source = source.replace(entry, entry + guard, 1)
        declaration = f"extern int {hook}(void);"
        if entry in source and declaration not in source:
            source = declaration + "\n" + source
    return source


def repair_renderer(source: str) -> str:
    # The software DMA walker owns GET. The old bring-up shim acknowledged a
    # submission immediately, causing the walker to skip to the newest PUT in
    # the middle of a draw and decode its remaining parameters as headers.
    replacements = {
        "            MEM32(pchan + 0x44) = edx & 0x0FFFFFFFu;":
        "            if (!nv2a_pb_dma_walk()) MEM32(pchan + 0x44) = edx & 0x0FFFFFFFu;",
        "    if (ecx) MEM32(ecx + 0x44) = MEM32(ebx);":
        "    if (ecx && !nv2a_pb_dma_walk()) MEM32(ecx + 0x44) = MEM32(ebx);",
        "    MEM32(edi + 0x30) = edi + 0x2C; /* GPU read pointer -> write sequence */":
        "    if (!nv2a_pb_dma_walk()) MEM32(edi + 0x30) = edi + 0x2C; /* legacy immediate completion */",
        "    if (esi) {\n        MEM32(esi + 0x30) = esi + 0x2C;\n    }":
        "    if (esi && !nv2a_pb_dma_walk()) {\n        MEM32(esi + 0x30) = esi + 0x2C;\n    }",
    }
    for old, new in replacements.items():
        if old in source:
            source = source.replace(old, new)
    wait_entry = "void sub_003BCBC0(void)\n{\n"
    wait_guard = "    if (xbox_RecompBlockOnGpuFence()) return;\n"
    if wait_entry in source and wait_guard not in source:
        source = source.replace(wait_entry, wait_entry + wait_guard)
    wait_declaration = "extern int xbox_RecompBlockOnGpuFence(void);"
    if wait_guard in source and wait_declaration not in source:
        source = source.replace('#include "recomp_funcs.h"',
                                '#include "recomp_funcs.h"\n' + wait_declaration, 1)
    channel_init = "    MEM32(ebx + 0x1C20) = ecx;"
    seed_get = "    if (ecx && nv2a_pb_dma_walk()) MEM32(ecx + 0x44) = MEM32(ebx + 0x24) & 0x0FFFFFFFu;"
    # Channel creation starts the pusher at the ring base. The lightweight
    # channel-allocation shim does not emulate that hardware initialization.
    if channel_init in source and seed_get not in source:
        source = source.replace(channel_init, channel_init + "\n" + seed_get, 1)
    semaphore_init = "    MEM32(ebx + 0x30) = eax;"
    register_sem = "    if (nv2a_pb_dma_walk()) nv2a_pb_set_semaphore_target(eax);"
    initialization = "    edi = eax + 0x20;\n" + semaphore_init + "\n    eax = edi + 0x20;"
    if "loc_003C107E: ;" in source and initialization in source:
        source = source.replace(initialization,
                                initialization.replace(semaphore_init, semaphore_init + "\n" + register_sem))
    declaration = "extern int nv2a_pb_dma_walk(void);"
    if "nv2a_pb_dma_walk()" in source and declaration not in source:
        source = source.replace('#include "recomp_funcs.h"',
                                '#include "recomp_funcs.h"\n' + declaration, 1)
    sem_declaration = "extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);"
    if register_sem in source and sem_declaration not in source:
        source = source.replace('#include "recomp_funcs.h"',
                                '#include "recomp_funcs.h"\n' + sem_declaration, 1)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", nargs="?", type=Path,
                        default=Path(__file__).resolve().parents[1] / "src/recomp/gen/recomp_stubs_unresolved.c")
    generated = parser.parse_args().generated
    # A regenerated split may place cloned entry-point fragments in different
    # chunks. Repairs identify their D3D code, rather than relying on a chunk ID.
    for path in dict.fromkeys((generated, *sorted(generated.parent.glob("*.c")))):
        if not path.exists():
            continue
        source = path.read_text(encoding="utf-8")
        repaired = repair_d3d8(repair_xmv(repair_renderer(source)))
        if repaired != source:
            path.write_text(repaired, encoding="utf-8", newline="\n")
            print(f"Prepared generated host integration: {path.name}")


if __name__ == "__main__":
    main()
