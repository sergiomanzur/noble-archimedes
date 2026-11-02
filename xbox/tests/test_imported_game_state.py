import importlib.util
import ctypes
import os
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('imported_state', ROOT / 'xbox/tools/imported_game_state.py')
state = importlib.util.module_from_spec(spec)
spec.loader.exec_module(state)


class GameStateTests(unittest.TestCase):
    def setUp(self):
        self.memory = {}

    def put(self, address, fmt, *values):
        for i, byte in enumerate(struct.pack('<' + fmt, *values)):
            self.memory[address + i] = byte

    def read(self, address, size):
        try:
            return bytes(self.memory[address + i] for i in range(size))
        except KeyError:
            return None

    def test_unmapped_memory_omits_fields(self):
        self.assertEqual(state.decode(self.read), {'player': {'active': False}})

    def test_player_and_scene(self):
        self.put(0xB234F4, 'bb', 5, 12)
        self.put(0xB23410, 'I', 0x28000)
        self.put(0xB41394, 'I', 0x100000)
        self.put(0x100000, 'I', 2)
        self.put(0x100144, 'h', 3)
        self.put(0x10014C, 'h', 2)
        self.put(0x1000EE, 'B', 0x18)
        self.put(0xB4129C + 2 * 0x1210 + 0x10, 'fff', 1, 2, 3)
        self.put(0xB4129C + 2 * 0x1210 + 0x2C, 'f', 0.5)
        self.put(0xB2CCC0 + 6 * 4, 'i', 81)
        result = state.decode(self.read)
        self.assertEqual((result['map'], result['room']), (5, 12))
        self.assertEqual(result['player'], {'active': True, 'pos': [1, 2, 3], 'yaw': 0.5, 'hp': 81})
        self.assertTrue(result['cutscene'])
        self.assertTrue(result['loading'])

    def test_corrupt_pointer_never_reads_outside_guest(self):
        self.put(0xB41394, 'I', 0xFFFFFFF0)
        calls = []
        def read(address, size):
            calls.append(address)
            return self.read(address, size)
        self.assertFalse(state.decode(read)['player']['active'])
        self.assertTrue(all(0x10000 <= a <= 0xFFFFFFFF for a in calls))

    def test_nonfinite_camera_omitted(self):
        self.put(0xC36B74, 'I', 0x200000)
        for offset in (0x20, 0x30, 0x40):
            self.put(0x200000 + offset, 'fff', float('nan'), 0, 0)
        self.assertNotIn('camera', state.decode(self.read))

    def test_europe_has_separate_addresses(self):
        self.put(0xB2AE4C, 'bb', 5, 8)
        self.assertEqual(state.decode(self.read, 'eu')['room'], 8)
        self.assertNotIn('room', state.decode(self.read, 'usa'))

    def test_short_reads_are_omitted(self):
        self.assertEqual(state.decode(lambda address, size: b'\x00'), {'player': {'active': False}})

    @unittest.skipUnless(os.name == 'nt', 'Windows process API')
    def test_process_reader_uses_full_width_host_addresses(self):
        buffer = ctypes.create_string_buffer(b'hello')
        reader = state.ProcessReader(os.getpid(), ctypes.addressof(buffer) - 0x10000)
        try:
            self.assertEqual(reader(0x10000, 5), b'hello')
            self.assertIsNone(reader(0xFFFFFFFFFFFFFFFF, 100))
        finally:
            reader.close()


if __name__ == '__main__':
    unittest.main()
