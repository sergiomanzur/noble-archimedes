"""Run the generated kickoff helper against a tiny fake register aperture."""
import pathlib
import importlib.util
import shutil
import subprocess
import tempfile
import unittest

XBOX = pathlib.Path(__file__).resolve().parents[1]


def helper_body(source):
    start = source.index("static void run_003bc9f4(void)")
    return source[start:source.index("\nvoid sub_003BC9F2(void)", start)]


class GpuSubmitRegression(unittest.TestCase):
    def test_repair_is_repeatable(self):
        spec = importlib.util.spec_from_file_location("prepare_generated", XBOX / "tools/prepare_generated.py")
        prepare = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(prepare)
        fixture = ('#include "recomp_funcs.h"\n'
                   '            MEM32(pchan + 0x44) = edx & 0x0FFFFFFFu;\n'
                   '    if (ecx) MEM32(ecx + 0x44) = MEM32(ebx);\n')
        repaired = prepare.repair_renderer(fixture)
        self.assertEqual(prepare.repair_renderer(repaired), repaired)

    def test_dma_submission_does_not_acknowledge_unexecuted_commands(self):
        generated = XBOX / "src/recomp/gen/recomp_stubs_unresolved.c"
        if not generated.exists():
            self.skipTest("The local generated game code is required for this fixture")
        source = generated.read_text()
        body = helper_body(source)
        init_start = source.index("loc_003C137C: ;") + len("loc_003C137C: ;")
        init = source[init_start:source.index("    MEM32(0x3C6AEC)", init_start)]
        chunk = (XBOX / "src/recomp/gen/recomp_0074.c").read_text()
        create_start = chunk.index("loc_003BB553: ;") + len("loc_003BB553: ;")
        post_create = chunk[create_start:chunk.index("    eax = MEM32(esp + 0x20)", create_start)]
        fence_start = chunk.index("loc_003BCBC0: ;")
        fence_start = chunk.index("    esi = MEM32(0x3C6F98);", fence_start)
        pre_fence = chunk[fence_start:chunk.index("    eax = MEM32(esi + 0x30);", fence_start)]
        setup = (XBOX / "src/recomp/gen/recomp_0075.c").read_text()
        sem_start = setup.index("    MEM32(ebx + 0x30) = eax;", setup.index("loc_003C107E: ;"))
        sem_init = setup[sem_start:setup.index("    eax = edi + 0x20;", sem_start)]
        harness = r'''
#include <stdint.h>
#include <stdio.h>
static uint32_t ram[0x500000 / 4];
#define MEM32(at) ram[(uint32_t)(at) / 4]
#define POP32(sp, dst) do { (dst) = MEM32(sp); (sp) += 4; } while(0)
static uint32_t eax, esi, edx, esp, ecx, ebx, edi;
static int dma;
static uint32_t sem_target;
int nv2a_pb_dma_walk(void) { return dma; }
void nv2a_pb_set_semaphore_target(uint32_t va) { sem_target = va; }
''' + body + '\nstatic void initialize_channel(void) {\n' + init + '\n}\n' + (
    '\nstatic void initialize_semaphore(void) {\n' + sem_init + '\n}\n'
    '\nstatic void post_create(void) {\n' + post_create + '\n}\n'
    '\nstatic void pre_fence(void) {\n' + pre_fence + '\n}\n') + r'''
int main(void) {
    dma = 1; ebx = edi = 0x100; eax = 0x40000;
    MEM32(0x3C6F98) = ebx;
    initialize_semaphore();
    if (sem_target != 0x40000) { puts("GPU semaphore was not registered"); return 5; }
    post_create(); pre_fence();
    if (MEM32(ebx + 0x30) != 0x40000) {
        puts("GPU completed fence aliases the CPU submitted counter"); return 6;
    }
    dma = 0; post_create(); pre_fence();
    if (MEM32(ebx + 0x30) != ebx + 0x2C) return 7;
    dma = 1; esi = 0x500; ebx = 0x100; esp = 0x30000;
    MEM32(esp + 0xC) = 0x20000;
    MEM32(ebx + 0x24) = 0x80180000;
    initialize_channel();
    if (MEM32(0x20000 + 0x44) != 0x180000) {
        puts("DMA channel did not start at the allocated ring base");
        return 4;
    }
    MEM32(0x3C6F98) = 0x100;
    MEM32(0x100 + 0x1C28) = 0x10000;
    esi = 0x100;
    MEM32(esi + 0x1C20) = 0x20000;
    MEM32(0x20000 + 0x44) = 0x180000;
    dma = 1; edx = 0x80181234; esp = 0x30000;
    run_003bc9f4();
    if (MEM32(0x20000 + 0x40) != 0x181234) return 1;
    if (MEM32(0x20000 + 0x44) != 0x180000) {
        puts("DMA submission advanced GET before the executor consumed it");
        return 2;
    }
    dma = 0; eax = 0; esi = 0x100; edx = 0x80182000; esp = 0x30000;
    run_003bc9f4();
    if (MEM32(0x20000 + 0x44) != 0x182000) return 3;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            compiler = shutil.which("gcc")
            if not compiler:
                self.skipTest("GCC is required to compile the local generated helper fixture")
            src = pathlib.Path(temp) / "gpu_submit.c"
            exe = pathlib.Path(temp) / "gpu_submit.exe"
            src.write_text(harness)
            subprocess.run([compiler, "-std=c11", str(src), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
