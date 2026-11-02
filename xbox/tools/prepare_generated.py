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
    "003B7280": 24,
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


GAME_STACK_BLOCK = re.compile(
    r'    \{\n'
    r'        extern int __cdecl printf\(const char \*, \.\.\.\);\n'
    r'        printf\((?P<args>"\[GAME_STACK\].*?)\);\n'
    r'    \}\n',
    re.DOTALL,
)


def repair_game_stack(source: str) -> str:
    """Make generated GAME_STACK prints opt-in host diagnostics.

    These printf calls are emitted by the recompiler into a few coroutine
    helpers.  They are useful for stack debugging but run thousands of times
    per second in normal gameplay.  The generated site keeps the same format
    and arguments, but only calls the host emitter when its flag is enabled.
    """
    def replace(match: re.Match) -> str:
        return ("    if (recomp_game_stack_trace_on) {\n"
                "        recomp_game_stack_log(__FUNCTION__, "
                + match.group("args") + ");\n"
                "    }\n")

    source, count = GAME_STACK_BLOCK.subn(replace, source)
    if not count:
        return source
    declaration = (
        "extern int recomp_game_stack_trace_on;\n"
        "extern void recomp_game_stack_log(const char *emitter, "
        "const char *format, ...);")
    if declaration not in source:
        marker = '#include "recomp_funcs.h"'
        if marker in source:
            source = source.replace(marker, marker + "\n" + declaration, 1)
        else:
            source = declaration + "\n" + source
    return source


CALL_PROFILE_BLOCK = """#elif defined(RECOMP_CALL_PROFILE)
/* Opt-in profiling: every direct call reports its target to the project, which
 * keeps the counts (a project defines recomp_call_profile_hit). Diffing the counts
 * of two situations shows which functions one of them runs and the other does not. */
void recomp_call_profile_hit(uint32_t va);
int noble_d3d8_guest_call(uint32_t va, uint32_t cleanup);
int noble_field_status_hook(void);
int noble_yuv_hook(void);
#define RECOMP_ABI_CALL(va, fn) do { recomp_call_profile_hit(va); (fn)(); } while(0)
#else
#define RECOMP_ABI_CALL(va, fn) (fn)()
#endif
"""

CALL_PROFILE_GATED = """#else
extern int recomp_call_profile_on;
void recomp_call_profile_enter(uint32_t va);
void recomp_call_profile_exit(uint32_t va);
#define RECOMP_ABI_CALL(va, fn) do { \
    if (recomp_call_profile_on) { \
        recomp_call_profile_enter((uint32_t)(va)); \
        (fn)(); \
        recomp_call_profile_exit((uint32_t)(va)); \
    } else { \
        (fn)(); \
    } \
} while(0)
#endif
"""


CALL_PROFILE_TEMPLATE_BLOCK = """#elif defined(RECOMP_CALL_PROFILE)
/* Opt-in profiling: every direct call reports its target to the project, which
 * keeps the counts (a project defines recomp_call_profile_hit). Diffing the counts
 * of two situations shows which functions one of them runs and the other does not. */
void recomp_call_profile_hit(uint32_t va);
#define RECOMP_ABI_CALL(va, fn) do { recomp_call_profile_hit(va); (fn)(); } while(0)
#else
#define RECOMP_ABI_CALL(va, fn) (fn)()
#endif
"""


def repair_call_profile_header(source: str) -> str:
    """Hook every RECOMP_ABI_CALL with enter/exit timing that is a no-op
    unless the runtime profiling flag is enabled.  Idempotent."""
    if "extern int recomp_call_profile_on;" in source:
        return source
    if CALL_PROFILE_BLOCK in source:
        return source.replace(CALL_PROFILE_BLOCK, CALL_PROFILE_GATED, 1)
    if CALL_PROFILE_TEMPLATE_BLOCK in source:
        return source.replace(CALL_PROFILE_TEMPLATE_BLOCK, CALL_PROFILE_GATED, 1)
    return source


def repair_42e40_counters(source: str) -> str:
    """Lightweight aggregate counters/timers inside translated sub_00042E40.

    Scoped strictly to that function; counters only, no per-iteration output.
    Strips any previous insertions first, so it is idempotent."""
    if "void sub_00042E40(void)" not in source:
        return source
    lines = source.splitlines(keepends=True)
    start_i = next(i for i, l in enumerate(lines)
                   if l.startswith("void sub_00042E40(void)"))
    end_i = next(i for i in range(start_i + 1, len(lines))
                 if lines[i].startswith("void sub_"))
    end_i -= 1
    stripped = [l for l in lines if "recomp_region_" not in l]
    decl = ("extern void recomp_region_count(unsigned id);\n"
            "extern void recomp_region_enter(unsigned id);\n"
            "extern void recomp_region_exit(unsigned id);")
    marker = '#include "recomp_funcs.h"'
    joined = "".join(stripped)
    if decl not in joined:
        joined = joined.replace(marker, marker + "\n" + decl, 1)
    stripped = joined.splitlines(keepends=True)
    start_i = next(i for i, l in enumerate(stripped)
                   if l.startswith("void sub_00042E40(void)"))
    end_i = next(i for i in range(start_i + 1, len(stripped))
                 if stripped[i].startswith("void sub_"))
    end_i -= 1
    body = "".join(stripped[start_i:end_i + 1])
    pairs = [
        ("loc_00042E40: ;\n",
         "loc_00042E40: ;\n    recomp_region_count(0); recomp_region_enter(0);\n"),
        ("loc_00042EF0: ;\n",
         "loc_00042EF0: ;\n    recomp_region_count(1);\n"),
        ("    xmm4 = XMM_MEM(eax); /* movups */\n    xmm0 = XMM_MEM(edx); /* movups */\n",
         "    recomp_region_count(2); recomp_region_enter(2);\n    xmm4 = XMM_MEM(eax); /* movups */\n    xmm0 = XMM_MEM(edx); /* movups */\n"),
        ("    xmm1 = XMM_MEM(ecx); /* movups */\n    xmm0 = XMM_MUL(xmm0, xmm1); /* mulps */\n    XMM_STORE(eax, xmm0); /* movups */\n",
         "    xmm1 = XMM_MEM(ecx); /* movups */\n    xmm0 = XMM_MUL(xmm0, xmm1); /* mulps */\n    XMM_STORE(eax, xmm0); /* movups */\n    recomp_region_exit(2);\n"),
        ("loc_00043346: ;\n",
         "loc_00043346: ;\n    recomp_region_count(3);\n"),
        ("    PUSH32(esp, 0x0004310Eu); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n",
         "    recomp_region_count(4); recomp_region_enter(4);\n    PUSH32(esp, 0x0004310Eu); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n    recomp_region_exit(4);\n"),
        ("    PUSH32(esp, 0x000431D3u); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n",
         "    recomp_region_count(5); recomp_region_enter(5);\n    PUSH32(esp, 0x000431D3u); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n    recomp_region_exit(5);\n"),
        ("    PUSH32(esp, 0x0004331Fu); RECOMP_ABI_CALL(0x000122A0u, sub_000122A0); /* call 0x000122A0 */\n",
         "    recomp_region_count(6); recomp_region_enter(6);\n    PUSH32(esp, 0x0004331Fu); RECOMP_ABI_CALL(0x000122A0u, sub_000122A0); /* call 0x000122A0 */\n    recomp_region_exit(6);\n"),
        ("    fp_push(MEMF(0x42D2FC)); /* fld float */\n",
         "    recomp_region_count(7); recomp_region_enter(7);\n    fp_push(MEMF(0x42D2FC)); /* fld float */\n"),
        ("    goto loc_000433E1;\n",
         "    goto loc_000433E1;\n    recomp_region_exit(7);\n"),
    ]
    for old, new in pairs:
        body = body.replace(old, new, 1)
    body = body.replace("    esp += 4; return; /* ret */\n",
                        "    recomp_region_exit(0);\n    esp += 4; return; /* ret */\n")
    out = stripped[:start_i] + [body] + stripped[end_i + 1:]
    return "".join(out)


def repair_42e40_pairtime(source: str) -> str:
    """Function-boundary pair timer hooks for sub_00042E40 (parent) and its
    sub_00012740 call sites (child).  Scoped; idempotent."""
    if "void sub_00042E40(void)" not in source:
        return source
    lines = [l for l in source.splitlines(keepends=True)
             if "recomp_pt_" not in l]
    joined = "".join(lines)
    decl = ("extern void recomp_pt_enter(unsigned id);\n"
            "extern void recomp_pt_exit(unsigned id);")
    marker = '#include "recomp_funcs.h"'
    if decl not in joined:
        joined = joined.replace(marker, marker + "\n" + decl, 1)
    lines = joined.splitlines(keepends=True)
    start_i = next(i for i, l in enumerate(lines)
                   if l.startswith("void sub_00042E40(void)"))
    end_i = next(i for i in range(start_i + 1, len(lines))
                 if lines[i].startswith("void sub_"))
    end_i -= 1
    body = "".join(lines[start_i:end_i + 1])
    body = body.replace(
        "loc_00042E40: ;\n",
        "loc_00042E40: ;\n    recomp_pt_enter(0);\n", 1)
    body = body.replace(
        "    esp += 4; return; /* ret */\n",
        "    recomp_pt_exit(0);\n    esp += 4; return; /* ret */\n")
    body = body.replace(
        "    PUSH32(esp, 0x0004310Eu); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n",
        "    recomp_pt_enter(1);\n    PUSH32(esp, 0x0004310Eu); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n    recomp_pt_exit(1);\n", 1)
    body = body.replace(
        "    PUSH32(esp, 0x000431D3u); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n",
        "    recomp_pt_enter(1);\n    PUSH32(esp, 0x000431D3u); RECOMP_ABI_CALL(0x00012740u, sub_00012740); /* call 0x00012740 */\n    recomp_pt_exit(1);\n", 1)
    out = lines[:start_i] + [body] + lines[end_i + 1:]
    return "".join(out)


def repair_camera(source: str) -> str:
    """Add mouse angular displacement only at the audited manual-camera sites.

    These two operands have already received the guest controller/time scale.
    All downstream guest subtraction, wrap, clamp, collision and ownership
    branches remain unchanged. Unknown translations fail before any write.
    """
    entry = "void sub_00344D00(void)\n{\n"
    if entry not in source:
        if "void sub_00344D00(" in source:
            raise ValueError("camera hook: unrecognized sub_00344D00 signature")
        return source
    if source.count(entry) != 1:
        raise ValueError("camera hook: duplicate sub_00344D00")
    start = source.index(entry)
    following = re.search(r"^void sub_", source[start + len(entry):], re.MULTILINE)
    end = start + len(entry) + following.start() if following else len(source)
    body = source[start:end]
    multiply = ("    fp_top() = RECOMP_FP_PC(fp_top() * MEMF(0x432C08)); "
                "/* fmul dword ptr [0x432c08] */\n")
    anchors = []
    for axis, address in enumerate(("00344D34", "00344D53")):
        hook = f"    fp_top() = RECOMP_FP_PC(noble_camera_adjust({axis}u, fp_top()));\n"
        if body.count(hook) > 1:
            raise ValueError("camera hook: duplicate angular adjustment")
        body = body.replace(hook, "")
        anchor = f"loc_{address}: ;\n" + multiply
        if body.count(f"loc_{address}: ;\n") != 1 or body.count(anchor) != 1:
            raise ValueError(f"camera hook: unrecognized angular site {address}")
        anchors.append((anchor, hook))
    if "noble_camera_adjust" in body:
        raise ValueError("camera hook: unrecognized prior adjustment")
    marker = '#include "recomp_funcs.h"'
    if marker not in source:
        raise ValueError("camera hook: missing generated header")
    for anchor, hook in anchors:
        body = body.replace(anchor, anchor + hook, 1)
    result = source[:start] + body + source[end:]
    declaration = "extern double noble_camera_adjust(unsigned axis, double original_delta);"
    if declaration not in result:
        result = result.replace(marker, marker + "\n" + declaration, 1)
    return result


def repair_wide_guest(source: str) -> str:
    """Place the opt-in visibility prototype before derived matrices are built."""
    entry = 'void sub_00012CD0(void)\n{\n'
    if entry not in source:
        if 'void sub_00012CD0(' in source:
            raise ValueError('wide guest hook: unrecognized function')
        return source
    if source.count(entry) != 1:
        raise ValueError('wide guest hook: duplicate function')
    start = source.index(entry)
    following = re.search(r'^void sub_', source[start + len(entry):], re.MULTILINE)
    end = start + len(entry) + following.start() if following else len(source)
    body = source[start:end]
    hook = '    MEMF(0x12FC2C0) = noble_wide_guest_adjust(MEMF(0x12FC2C0));\n'
    hook_y = '    MEMF(0x12FC2D4) = noble_wide_guest_adjust_y(MEMF(0x12FC2D4));\n'
    gated = ('    if (noble_wide_guest_active()) {\n'
             '        MEMF(0x12FC2C0) = noble_wide_guest_adjust(MEMF(0x12FC2C0));\n'
             '        MEMF(0x12FC2D4) = noble_wide_guest_adjust_y(MEMF(0x12FC2D4));\n'
             '    }\n')
    if body.count(gated) > 1:
        raise ValueError('wide guest hook: duplicate gated hook')
    body = body.replace(gated, '')
    if body.count(hook) > 1:
        raise ValueError('wide guest hook: duplicate hook')
    body = body.replace(hook, '')
    if body.count(hook_y) > 1:
        raise ValueError('wide guest hook: duplicate y hook')
    body = body.replace(hook_y, '')
    anchor = 'loc_00012DD9: ;\n    ecx = MEM32(esp + 0x38);\n'
    if body.count(anchor) != 1 or 'noble_wide_guest_adjust' in body:
        raise ValueError('wide guest hook: unrecognized anchor')
    marker = '#include "recomp_funcs.h"'
    if marker not in source:
        raise ValueError('wide guest hook: missing header')
    body = body.replace(anchor, 'loc_00012DD9: ;\n' + gated + '    ecx = MEM32(esp + 0x38);\n', 1)
    result = source[:start] + body + source[end:]
    declaration = 'extern float noble_wide_guest_adjust(float original);'
    if declaration not in result:
        result = result.replace(marker, marker + '\n' + declaration, 1)
    declaration_y = 'extern float noble_wide_guest_adjust_y(float original);'
    if declaration_y not in result:
        result = result.replace(marker, marker + '\n' + declaration_y, 1)
    declaration_active = 'extern int noble_wide_guest_active(void);'
    if declaration_active not in result:
        result = result.replace(marker, marker + '\n' + declaration_active, 1)
    return result


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
        repaired = repair_wide_guest(repair_camera(repair_d3d8(repair_xmv(repair_renderer(repair_game_stack(repair_42e40_pairtime(repair_42e40_counters(source))))))))
        if repaired != source:
            path.write_text(repaired, encoding="utf-8", newline="\n")
            print(f"Prepared generated host integration: {path.name}")
    for header in (generated.parent / "recomp_types.h",
                   Path(__file__).resolve().parents[2]
                   / "tools/xboxrecomp/templates/runtime/recomp_types.h"):
        if not header.exists():
            continue
        source = header.read_text(encoding="utf-8")
        repaired = repair_call_profile_header(source)
        if repaired != source:
            header.write_text(repaired, encoding="utf-8", newline="\n")
            print(f"Prepared call-profile header: {header.name}")


if __name__ == "__main__":
    main()
