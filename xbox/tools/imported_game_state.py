"""Read-only Xbox state diagnostics adapted from jmafort/CastlevaniaCOD-PC.

Address/layout research: jmafort, Pazzi7054, Neguchads. GPL-2.0; see
../third_party/castlevania-cod-pc/LICENSE and IMPORT.json. This adapter reads
our existing host without linking either runtime or modifying guest memory.
"""
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import struct
import time

PROFILES = json.loads(Path(__file__).with_name('imported_game_profiles.json').read_text())


def decode(reader, region='usa'):
    """Decode a best-effort sample via reader(guest_address, size) -> bytes/None.

    Samples taken from another running process are not atomic frame snapshots.
    Unreadable pointers and nonfinite floats are omitted, never fabricated.
    """
    p = PROFILES[region]

    def read(address, fmt):
        size = struct.calcsize('<' + fmt)
        if address < 0x10000 or address > 0x100000000 - size:
            return None
        raw = reader(address, size)
        if raw is None or len(raw) != size:
            return None
        values = struct.unpack('<' + fmt, raw)
        if any(isinstance(v, float) and not math.isfinite(v) for v in values):
            return None
        return values[0] if len(values) == 1 else list(values)

    result = {'player': {'active': False}}
    room = read(p['MapRoom'], 'bb')
    if room is not None:
        result.update(map=room[0], room=room[1])
    player = result['player']
    handle = read(p['Player'], 'I')
    if handle:
        index = read(handle + 0x144, 'h')
        flags = read(handle + 0xEE, 'B')
        player['active'] = (index is not None and 0 <= index < 128 and
                            read(handle + 0x146 + index * 2, 'h') == 2 and
                            flags is not None and (flags & 0x18) == 0x18)
        record = read(handle, 'I')
        if record is not None and record < 128:
            address = p['Records'] + record * 0x1210
            pos, yaw = read(address + 0x10, 'fff'), read(address + 0x2C, 'f')
            if pos is not None and yaw is not None:
                player.update(pos=pos, yaw=yaw)
    for key, address in [('hp', p['Attributes'] + 24), ('hp_max', p['Attributes'] + 20),
                         ('mp', p['Attributes'] + 128), ('mp_max', p['Attributes'] + 124),
                         ('exp', p['Attributes'] + 12), ('gold', p['Gold'])]:
        value = read(address, 'i')
        if value is not None:
            player[key] = value
    view = read(p['CameraView'], 'I')
    if view:
        angles = read(view + 0x20, 'fff')
        target, pos = read(view + 0x30, 'fff'), read(view + 0x40, 'fff')
        if angles is not None and target is not None and pos is not None:
            result['camera'] = dict(pos=pos, target=target, angles=angles,
                                    heading=math.atan2(target[0] - pos[0], target[2] - pos[2]))
            descriptor = read(p['CameraDescriptor'], 'I')
            callback = read(descriptor + 0x1D0, 'I') if descriptor else None
            if callback is not None:
                result['camera']['special'] = callback in (p['TitleCallback'], p['DebugCallback'])
    scene = read(p['Scene'], 'I')
    if scene is not None:
        result.update(scene=scene, cutscene=bool(scene & 0x20000), loading=bool(scene & 0x8000))
    menu = read(p['Menu'], 'I')
    if menu is not None:
        result['menu'] = bool(menu)
    modal = read(p['ModalFlags'], 'I')
    flags = read(modal, 'I') if modal else None
    if flags is not None:
        result['modal'] = bool(flags & 0x10000)
    mode = read(p['GameInfo'], 'I')
    if mode is not None:
        result['mode'] = mode
    return result


class ProcessReader:
    def __init__(self, pid, offset=0):
        if os.name != 'nt':
            raise OSError('Live process reading requires Windows')
        from ctypes import wintypes as w
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.kernel.OpenProcess.restype = w.HANDLE
        self.kernel.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.kernel.ReadProcessMemory.restype = w.BOOL
        self.kernel.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.kernel.OpenProcess(0x10, False, pid)  # PROCESS_VM_READ only
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        self.offset = offset

    def __call__(self, address, size):
        address += self.offset
        if address < 0 or address > (1 << (8 * ctypes.sizeof(ctypes.c_void_p))) - size:
            return None
        buffer = ctypes.create_string_buffer(size)
        count = ctypes.c_size_t()
        if not self.kernel.ReadProcessMemory(self.handle, address, buffer, size, ctypes.byref(count)):
            return None
        return buffer.raw if count.value == size else None

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--region', choices=PROFILES, default='usa', help='must match the loaded Xbox XBE')
    parser.add_argument('--memory-offset', type=lambda s: int(s, 0), default=0,
                        help='g_xbox_mem_offset for the current host; zero for identity mapping')
    parser.add_argument('--samples', type=int, default=1)
    parser.add_argument('--interval', type=float, default=1.0)
    args = parser.parse_args()
    if args.pid <= 0 or not 1 <= args.samples <= 3600 or not math.isfinite(args.interval) or args.interval < 0:
        parser.error('use a positive PID, 1..3600 samples and a finite nonnegative interval')
    reader = ProcessReader(args.pid, args.memory_offset)
    try:
        for index in range(args.samples):
            sample = decode(reader, args.region)
            sample.update(sample=index, time=time.time(), region=args.region)
            print(json.dumps(sample, allow_nan=False), flush=True)
            if index + 1 < args.samples:
                time.sleep(args.interval)
    finally:
        reader.close()


if __name__ == '__main__':
    main()
