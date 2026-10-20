"""Host D3D hooks must survive regeneration, once per real entry point."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "prepare_generated", Path(__file__).resolve().parents[1] / "tools/prepare_generated.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class D3D8Hooks(unittest.TestCase):
    def test_regeneration_replays_real_entries_once(self):
        self.assertTrue(hasattr(prepare, "repair_d3d8"), "D3D8 hooks are not replayed")
        fixture = '#include "recomp_funcs.h"\n'
        for va in ("003BB4C0", "003B6520", "003B6FB0", "003B8A50", "003B7A60",
                   "003BA740", "003BA8B0", "003BA9F0", "003B6B80", "003B75F0"):
            fixture += f"void sub_{va}(void)\n{{\n    /* guest body */\n}}\n"
        result = prepare.repair_d3d8(fixture)
        self.assertEqual(result, prepare.repair_d3d8(result))
        for va, cleanup in (("003B8A50", 16), ("003B7A60", 12), ("003BA740", 20),
                            ("003BA8B0", 24), ("003BA9F0", 16)):
            self.assertIn(f"noble_d3d8_guest_call(0x{va}u, {cleanup}u)", result)
        self.assertEqual(result.count("extern int noble_d3d8_guest_call("), 1)

    def test_remove_obsolete_guessed_entry_hooks(self):
        self.assertTrue(hasattr(prepare, "repair_d3d8"), "D3D8 hooks are not replayed")
        fixture = "void sub_003B7B60(void)\n{\n    if (noble_d3d8_guest_call(0x003B7B60u, 12u)) return;\n}\n"
        self.assertNotIn("noble_d3d8_guest_call(0x003B7B60", prepare.repair_d3d8(fixture))
