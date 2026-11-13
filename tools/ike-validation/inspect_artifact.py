"""Offline ELF check; never runs a binary or accesses the device."""
import hashlib
import json
from pathlib import Path
import struct
import sys

def inspect_elf(data):
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected little-endian ELF32')
    machine = struct.unpack_from('<H', data, 18)[0]
    flags = struct.unpack_from('<I', data, 36)[0]
    if machine != 40 or flags & 0xff000000 != 0x05000000 or flags & 0x400:
        raise ValueError('expected ARM EABI5 without hard-float ABI')
    offset = struct.unpack_from('<I', data, 28)[0]
    size, count = struct.unpack_from('<HH', data, 42)
    if size < 32 or offset + size * count > len(data):
        raise ValueError('invalid program header bounds')
    for i in range(count):
        kind = struct.unpack_from('<I', data, offset + i * size)[0]
        if kind in (2, 3): raise ValueError('dynamic loader/dependencies are forbidden')
    return {'abi': 'ARM EABI5 soft-float', 'bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest(), 'static': True}

if __name__ == '__main__':
    print(json.dumps({str(p): inspect_elf(Path(p).read_bytes()) for p in sys.argv[1:]}, indent=2))
