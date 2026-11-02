import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('noble_backend', ROOT / 'xbox/tools/jmafort_backend.py')
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)


class NobleIntegration(unittest.TestCase):
    def test_library_and_target_names_agree(self):
        for source in ('CMakeLists.txt', 'config/game.toml', 'config/game_eu.toml', 'src/common/game_info.cpp'):
            data = backend.branded_source(source, (backend.SNAPSHOT / source).read_bytes()).decode()
            if source == 'CMakeLists.txt':
                self.assertIn('add_xbe_image(NobleArchimedes ', data)
                self.assertIn('add_xbe_image(NobleArchimedesEU ', data)
                self.assertIn('NobleArchimedesLauncher', data)
                self.assertIn('noble-branding.cmake', data)
                self.assertIn('Castlevania is a trademark of Konami.', data)
                self.assertNotIn('NobleArchimedes is a trademark', data)
                self.assertIn('0.9.0b', data)
            elif source.endswith('.toml'):
                self.assertIn('library = "NobleArchimedes' + ('EU' if '_eu' in source else '') + '.dll"', data)
            else:
                self.assertIn('CV_PICK("NobleArchimedes", "NobleArchimedesEU")', data)
                self.assertIn('.title = L"Noble Archimedes"', data)

    def test_source_sync_updates_own_output_but_preserves_manual_edits(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshot, work, overlay = root / 'source', root / 'work', root / 'overlay'
            snapshot.mkdir()
            overlay.mkdir()
            (snapshot / 'file.txt').write_text('upstream\n')
            manifest = {'files': {'file.txt': 'unused'}}
            backend.sync_sources(manifest, snapshot, work, overlay)
            (overlay / 'file.txt').write_text('branded\n')
            backend.sync_sources(manifest, snapshot, work, overlay)
            self.assertEqual((work / 'file.txt').read_text(), 'branded\n')
            (work / 'file.txt').write_text('manual edit\n')
            with self.assertRaisesRegex(RuntimeError, 'edited'):
                backend.sync_sources(manifest, snapshot, work, overlay)
            self.assertEqual((work / 'file.txt').read_text(), 'manual edit\n')

    def test_primary_root_build_keeps_ps2_opt_in(self):
        source = (ROOT / 'CMakeLists.txt').read_text()
        self.assertIn('NOBLE_EXPERIMENTAL_PS2', source)
        self.assertIn('LANGUAGES NONE', source)


if __name__ == '__main__':
    unittest.main()
