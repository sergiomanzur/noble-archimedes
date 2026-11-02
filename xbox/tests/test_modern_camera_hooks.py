"""Native camera preparation must replay narrowly and reject unknown translations."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('prepare_generated', Path(__file__).resolve().parents[1] / 'tools/prepare_generated.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
MULTIPLY = '    fp_top() = RECOMP_FP_PC(fp_top() * MEMF(0x432C08)); /* fmul dword ptr [0x432c08] */\n'
FIXTURE = ('#include "recomp_funcs.h"\nvoid sub_00344D00(void)\n{\n'
           'loc_00344D34: ;\n' + MULTIPLY + '    /* pitch subtract and clamp stay guest owned */\n'
           'loc_00344D53: ;\n' + MULTIPLY + '    /* yaw wrap and collision stay guest owned */\n}\n'
           'void sub_00315690(void)\n{\n' + MULTIPLY + '}\n')

class CameraHooks(unittest.TestCase):
    def repair(self, source):
        self.assertTrue(hasattr(prepare, 'repair_camera'), 'camera hook repair missing')
        return prepare.repair_camera(source)

    def test_two_angular_operands_only_and_idempotence(self):
        result = self.repair(FIXTURE)
        self.assertEqual(result, self.repair(result))
        self.assertEqual(result.count('RECOMP_FP_PC(noble_camera_adjust('), 2)
        for axis, label in enumerate(('00344D34', '00344D53')):
            self.assertIn('loc_' + label + ': ;\n' + MULTIPLY + f'    fp_top() = RECOMP_FP_PC(noble_camera_adjust({axis}u, fp_top()));\n', result)
        # Removing just our additions must reproduce every original instruction.
        for axis in (0, 1):
            result = result.replace(f'    fp_top() = RECOMP_FP_PC(noble_camera_adjust({axis}u, fp_top()));\n', '')
        result = result.replace('extern double noble_camera_adjust(unsigned axis, double original_delta);\n', '')
        self.assertEqual(result, FIXTURE)

    def test_unknown_or_duplicate_anchor_rejected_without_partial_patch(self):
        for source in (FIXTURE.replace('0x432C08', '0x432C09', 1),
                       FIXTURE.replace('sub_00344D00(void)\n{', 'sub_00344D00(void) {'),
                       FIXTURE.replace('loc_00344D53: ;', 'loc_00344D54: ;'),
                       FIXTURE.replace('loc_00344D34: ;', 'loc_00344D34: ;\n' + MULTIPLY + 'loc_00344D34: ;')):
            with self.assertRaises(ValueError):
                self.repair(source)

    def test_unrelated_chunks_are_unchanged(self):
        self.assertEqual(self.repair('void sub_00315690(void)\n{\n' + MULTIPLY + '}\n'), 'void sub_00315690(void)\n{\n' + MULTIPLY + '}\n')

if __name__ == '__main__':
    unittest.main()
