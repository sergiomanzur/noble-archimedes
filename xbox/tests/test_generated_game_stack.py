import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "xbox" / "tools"))
import prepare_generated


SAMPLE = """#include "recomp_funcs.h"

void sub_00044140(void)
{
    {
        extern int __cdecl printf(const char *, ...);
        printf("[GAME_STACK] push_frame depth=%u (state=%u) ret=0x%08X\n", MEM32(0xA4E2D8), MEM32(MEM32(0xA4E2D8) * 4 + 0xA4E2DC), MEM32(esp));
    }
    esp += 4; return;
}
"""


class GameStackPatchTest(unittest.TestCase):
    def test_unconditional_print_becomes_opt_in(self):
        repaired = prepare_generated.repair_game_stack(SAMPLE)
        self.assertIn("if (recomp_game_stack_trace_on)", repaired)
        self.assertIn("recomp_game_stack_log(__FUNCTION__", repaired)
        self.assertIn("extern int recomp_game_stack_trace_on;", repaired)
        self.assertNotIn('printf("[GAME_STACK]', repaired)
        self.assertEqual(repaired, prepare_generated.repair_game_stack(repaired))

    def test_unrelated_printf_is_untouched(self):
        source = SAMPLE + 'void f(void) { printf("ordinary\\n"); }\n'
        repaired = prepare_generated.repair_game_stack(source)
        self.assertIn('printf("ordinary\\n")', repaired)


if __name__ == "__main__":
    unittest.main()
