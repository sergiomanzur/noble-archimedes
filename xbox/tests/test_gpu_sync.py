"""Exercise the real GPU fence waiter with simulated asynchronous completion."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

XBOX = pathlib.Path(__file__).resolve().parents[1]


class GpuFenceWait(unittest.TestCase):
    def test_wait_flushes_and_preserves_guest_state(self):
        compiler = shutil.which("gcc")
        if not compiler:
            self.skipTest("GCC is required for this native fixture")
        module = XBOX / "src/gpu_sync.c"
        self.assertTrue(module.exists(), "Host GPU fence bridge is missing")
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "recomp/gen").mkdir(parents=True)
            (root / "windows.h").write_text('void Sleep(unsigned long milliseconds);\n')
            (root / "recomp/gen/recomp_types.h").write_text(r'''
#include <stdint.h>
extern uint32_t ram[], g_esp, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
#define MEM32(at) ram[(uint32_t)(at) / 4]
#define PUSH32(sp, v) do { (sp) -= 4; MEM32(sp) = (v); } while (0)
void sub_003BCB00(void);
void sub_003BC9E0(void);
''')
            fixture = root / "fixture.c"
            fixture.write_text(r'''
#include <recomp/gen/recomp_types.h>
uint32_t ram[0x500000/4], g_esp, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp;
static int dma = 1, kicks, fences, sleeps, bad;
int nv2a_pb_dma_walk(void) { return dma; }
void sub_003BCB00(void) { fences++; MEM32(0x100+0x2C) += 2; g_esp += 8; g_esi=99; }
void sub_003BC9E0(void) { kicks++; g_esp+=4; g_ebx=88; g_edi=77; g_ebp=66; g_seh_ebp=55; }
void Sleep(unsigned long ms) {
    (void)ms;
    if (!kicks || MEM32(0x40000) != 3) bad=1;
    sleeps++; MEM32(0x40000)=5;
}
int xbox_RecompBlockOnGpuFence(void);
int main(void) {
    MEM32(0x3C6F98)=0x100; MEM32(0x100+0x30)=0x40000;
    MEM32(0x100+0x2C)=5; MEM32(0x40000)=3;
    g_esp=0x30000; MEM32(g_esp+4)=5; MEM32(g_esp+8)=8;
    g_ebx=1;g_esi=2;g_edi=3;g_ebp=4;g_seh_ebp=5;
    if (!xbox_RecompBlockOnGpuFence()) return 1;
    if (bad || sleeps!=1 || kicks!=1 || fences!=1) return 2;
    if (g_esp!=0x3000C || g_ebx!=1 || g_esi!=2 || g_edi!=3 || g_ebp!=4 || g_seh_ebp!=5) return 3;
    g_esp=0x30000;
    if (!xbox_RecompBlockOnGpuFence() || g_esp!=0x3000C || kicks!=1 || sleeps!=1 || fences!=1) return 5;
    dma=0; g_esp=0x30000;
    if (xbox_RecompBlockOnGpuFence() || g_esp!=0x30000) return 4;
    return 0;
}
''')
            exe = root / "gpu_sync.exe"
            subprocess.run([compiler, "-std=c11", "-DSleep=gpu_test_sleep", "-I", str(root), str(module), str(fixture), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)])
            self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
