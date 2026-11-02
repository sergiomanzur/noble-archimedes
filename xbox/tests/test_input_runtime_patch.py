"""Tracked provider patch must apply once and preserve incompatible edits."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "prepare_input_runtime", Path(__file__).resolve().parents[1] / "tools/prepare_input_runtime.py")
prepare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(prepare)

PATCH = """diff --git a/input.c b/input.c
--- a/input.c
+++ b/input.c
@@ -1 +1 @@
-legacy input
+provider input
"""

class InputRuntimePatch(unittest.TestCase):
    def test_once_idempotent_and_incompatible_edits_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            target = root / "input.c"
            target.write_text("legacy input\n")
            patch = root / "provider.patch"
            patch.write_text(PATCH)
            self.assertTrue(prepare.ensure_provider(root, patch))
            self.assertEqual(target.read_text(), "provider input\n")
            self.assertFalse(prepare.ensure_provider(root, patch))
            target.write_text("unrelated local implementation\n")
            with self.assertRaises(RuntimeError):
                prepare.ensure_provider(root, patch)
            self.assertEqual(target.read_text(), "unrelated local implementation\n")

if __name__ == "__main__":
    unittest.main()
