"""Launcher branding contracts that can run without a Windows graphics build."""
import ast
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
OVERLAY = ROOT / 'xbox/noble/overlay/src/launcher'


def cpp_text(expression):
    return ''.join(ast.literal_eval(token) for token in re.findall(r'"(?:\\.|[^"\\])*"', expression))


class NobleLauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (OVERLAY / 'launcher.cpp').read_text(encoding='utf-8')
        cls.words = (OVERLAY / 'words.h').read_text(encoding='utf-8')

    def test_every_literal_translation_key_has_a_complete_row(self):
        rows = []
        table = self.words.split('inline constexpr Row kRows[] = {', 1)[1].split('// every row has each language', 1)[0]
        for row in re.findall(r'\{\{(.*?)\}\}', table, flags=re.S):
            # Commas inside quoted text must not split a language column.
            columns = re.split(r',(?=(?:[^"\\]|\\.|"(?:\\.|[^"\\])*")*$)', row)
            values = [cpp_text(column) for column in columns]
            self.assertEqual(len(values), 7, values)
            self.assertTrue(all(values), values)
            self.assertTrue(all(value.count('{}') == values[0].count('{}') for value in values), values)
            rows.append(values[0])
        keys = re.findall(r'\b(?:T|F)\(((?:\s*"(?:\\.|[^"\\])*")+)', self.source)
        for key in keys:
            self.assertIn(cpp_text(key), rows)
        self.assertIn('Launcher music', rows)

    def test_launcher_art_is_independent_of_game_texture_pack(self):
        self.assertNotIn('files_.logo', self.source)
        self.assertNotIn('LoadLogo', self.source)
        self.assertNotIn('b0f0108366aad095', self.source)
        for filename in ('launcher-background.png', 'noble-icon.png', 'launcher-theme.wav'):
            self.assertIn('L"branding" / L"' + filename + '"', self.source)
        # Optional textures and PS2 button icon detection still work.
        self.assertIn('pictures++;', self.source)
        self.assertIn('27818fa40a1117af-66279948575c23d7-', self.source)

    def test_music_choice_is_persisted_and_playback_stops_after_launch(self):
        self.assertIn('rest["launcher_music"].value_or(launcher_music)', self.source)
        self.assertIn('rest.insert_or_assign("launcher_music", launcher_music)', self.source)
        self.assertIn('std::to_string(s.launcher_music)', self.source)
        self.assertIn('SND_ASYNC | SND_LOOP | SND_FILENAME | SND_NODEFAULT', self.source)
        play = self.source.split('    bool Play() {', 1)[1].split('    void AddImage()', 1)[0]
        self.assertLess(play.index('CreateProcessW'), play.index('PlaySoundW(nullptr'))
        self.assertIn('music_suspended_ = true', play)
        self.assertIn('launcher.Shutdown();', self.source)

    def test_both_releases_launch_branded_executables(self):
        self.assertIn('which > 0 ? L"NobleArchimedesEU.exe" : L"NobleArchimedes.exe"', self.source)
        self.assertNotIn('"Castlevania.exe', self.source)
        self.assertNotIn('"Castlevania.exe', self.words)
        self.assertIn('kReleaseCount', self.source)
        self.assertIn('Required input: Castlevania: Curse of Darkness (Xbox)', self.source)


if __name__ == '__main__':
    unittest.main()
