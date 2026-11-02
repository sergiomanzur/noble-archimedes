import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('backend', ROOT / 'xbox/tools/jmafort_backend.py')
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)


class BackendTests(unittest.TestCase):
    def test_snapshot_matches_manifest(self):
        manifest = backend.verify_snapshot()
        self.assertEqual(len(manifest['files']), 60)
        self.assertEqual(manifest['sdk_commit'], '91146ff6e2a218b19c1109dc665d06020472ab57')

    def test_changed_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'file').write_bytes(b'changed')
            (root / 'IMPORT.json').write_text(json.dumps({'files': {'file': hashlib.sha256(b'original').hexdigest()}}))
            with self.assertRaisesRegex(RuntimeError, 'file'):
                backend.verify_snapshot(root)

    def test_inventory_survives_git_line_ending_conversion(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'file').write_bytes(b'line\r\n')
            (root / 'IMPORT.json').write_text(json.dumps({
                'hash_normalization': 'CRLF to LF',
                'files': {'file': hashlib.sha256(b'line\n').hexdigest()}}))
            backend.verify_snapshot(root)

    def test_unknown_xbe_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'default.xbe'
            path.write_bytes(b'not an XBE')
            with self.assertRaisesRegex(RuntimeError, 'Unsupported'):
                backend.identify_xbe(path)

    def test_profile_metadata_still_matches_donor(self):
        import re
        text = (backend.SNAPSHOT / 'src/common/release.h').read_text()
        profiles = json.loads((ROOT / 'xbox/tools/imported_game_profiles.json').read_text())
        for name, usa, eu in re.findall(r'uint32_t k(\w+) = CV_PICK\((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)', text):
            self.assertEqual(profiles['usa'][name], int(usa, 16))
            self.assertEqual(profiles['eu'][name], int(eu, 16))

    def test_eu_runtime_has_its_own_asset_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / 'build'
            assets = root / 'eu-assets'
            build.mkdir()
            assets.mkdir()
            (build / 'compiler').mkdir()
            (build / 'CastlevaniaEU.exe').write_bytes(b'eu exe')
            (build / 'CastlevaniaLauncher.exe').write_bytes(b'launcher')
            (build / 'CastlevaniaEU.dll').write_bytes(b'local game code')
            play = backend.stage_eu(build, assets)
            self.assertEqual((play / 'assets').resolve(), assets.resolve())
            self.assertEqual((play / 'compiler').resolve(), (build / 'compiler').resolve())
            self.assertEqual((play / 'CastlevaniaEU.exe').read_bytes(), b'eu exe')
            self.assertEqual(backend.stage_eu(build, assets), play)


if __name__ == '__main__':
    unittest.main()
