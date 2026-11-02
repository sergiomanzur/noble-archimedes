import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('prepare_generated', Path(__file__).resolve().parents[1] / 'tools/prepare_generated.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
FIXTURE = ('#include "recomp_funcs.h"\nvoid sub_00012CD0(void)\n{\n'
           'loc_00012DD9: ;\n    ecx = MEM32(esp + 0x38);\n}\n')

class WideGuestHooks(unittest.TestCase):
    def test_single_idempotent_hook(self):
        result = prepare.repair_wide_guest(FIXTURE)
        self.assertEqual(result, prepare.repair_wide_guest(result))
        self.assertEqual(result.count('noble_wide_guest_adjust(MEMF(0x12FC2C0))'), 1)
        self.assertIn('loc_00012DD9: ;\n    if (noble_wide_guest_active()) {\n', result)
        self.assertIn('MEMF(0x12FC2D4) = noble_wide_guest_adjust_y(MEMF(0x12FC2D4));', result)

    def test_wrong_anchor_rejected(self):
        with self.assertRaises(ValueError):
            prepare.repair_wide_guest(FIXTURE.replace('loc_00012DD9:', 'loc_00012DDA:'))
        with self.assertRaises(ValueError):
            prepare.repair_wide_guest(FIXTURE.replace('ecx = MEM32(esp + 0x38);', 'ecx = MEM32(esp + 0x3C);'))

    def test_unrelated_file_unchanged(self):
        self.assertEqual(prepare.repair_wide_guest('void sub_1234(void) {}'), 'void sub_1234(void) {}')

if __name__ == '__main__': unittest.main()
