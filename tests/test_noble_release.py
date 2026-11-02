"""The runtime release is an allowlist, even in a contaminated build tree."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / 'tools/release_noble.py'


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location('release_noble', SCRIPT)
        self.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.mod)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.build = self.root / 'build'
        self.sdk = self.root / 'sdk'
        self.redist = self.root / 'redist'
        for name in self.mod.ROOT_REQUIRED:
            self.put(self.root / name)
        for name in self.mod.BUILD_REQUIRED:
            self.put(self.build / name)
        for name in self.mod.SDK_REQUIRED:
            self.put(self.sdk / name)
        for name in self.mod.REDIST_FILES:
            self.put(self.redist / name)
        self.put(self.build / 'version.txt', '0.9.0b')
        self.put(self.build / '_deps/sdl3-src/LICENSE.txt')
        self.put(self.build / '_deps/sdl3-src/vendor/nested/COPYING')

    def put(self, path, text='fixture'):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def package(self):
        return self.mod.package(self.root, self.build, self.sdk, self.redist)

    def test_allowlist_and_manifest_and_unique_outputs(self):
        for bad in ['NobleArchimedes.dll', 'Castlevania.exe', 'game.iso',
                    'compiler/work/generated.cpp', 'compiler/work/default.xbe',
                    'compiler/Castlevania.lib', 'branding/game.png', 'saves/save.dat']:
            self.put(self.build / bad, 'DO NOT SHIP')
        archive = self.package()
        with zipfile.ZipFile(archive) as z:
            names = z.namelist()
            self.assertTrue('assets/' in names and 'textures/' in names)
            self.assertFalse(any('Castlevania' in x or '/work/' in x or x.endswith('.iso') for x in names))
            self.assertFalse('NobleArchimedes.dll' in names)
            self.assertFalse('branding/game.png' in names)
            self.assertIn('compiler/NobleArchimedes.lib', names)
            self.assertIn('licenses/dependencies/sdl3-src/vendor/nested/COPYING', names)
            for name in self.mod.REDIST_FILES:
                self.assertIn(name, names)
                self.assertIn('compiler/' + name, names)
            manifest = json.loads(z.read('manifest.json'))
            self.assertEqual(manifest['regions'], ['USA'])
            for name, digest in manifest['sha256'].items():
                self.assertEqual(digest, hashlib.sha256(z.read(name)).hexdigest())
        self.assertNotEqual(archive, self.package())
        self.assertTrue(archive.is_file())

    def test_missing_required_input_fails_without_output(self):
        (self.build / 'compiler/NobleArchimedes.lib').unlink()
        with self.assertRaisesRegex(ValueError, 'NobleArchimedes.lib'):
            self.package()
        self.assertFalse((self.root / 'output/releases').exists())

    def test_version_must_match(self):
        self.put(self.build / 'version.txt', '0.3.0')
        with self.assertRaisesRegex(ValueError, 'version'):
            self.package()

    def test_eu_requires_both_host_and_library(self):
        self.put(self.build / 'NobleArchimedesEU.exe')
        with zipfile.ZipFile(self.package()) as z:
            self.assertNotIn('NobleArchimedesEU.exe', z.namelist())
        self.put(self.build / 'compiler/NobleArchimedesEU.lib')
        self.put(self.build / 'compiler/config/game_eu.toml')
        with zipfile.ZipFile(self.package()) as z:
            manifest = json.loads(z.read('manifest.json'))
            self.assertEqual(manifest['regions'], ['USA', 'EU'])
            self.assertFalse(manifest['eu_qualified'])

    def test_final_validation_rejects_unknown_dll(self):
        with self.assertRaisesRegex(ValueError, 'DLL'):
            self.mod.validate_payload({'NobleArchimedes.dll': b'bad'})

    def test_missing_dependency_notices_rejected(self):
        (self.build / '_deps/sdl3-src/LICENSE.txt').unlink()
        (self.build / '_deps/sdl3-src/vendor/nested/COPYING').unlink()
        with self.assertRaisesRegex(ValueError, 'dependency license'):
            self.package()

    def test_symlink_inputs_rejected(self):
        path = self.build / 'compiler/clang.exe'
        path.unlink()
        try:
            path.symlink_to(self.build / 'NobleArchimedes.exe')
        except OSError:
            self.skipTest('symlinks unavailable')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            self.package()


if __name__ == '__main__':
    unittest.main()
