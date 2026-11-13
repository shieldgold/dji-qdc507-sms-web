import struct
import unittest
from inspect_artifact import inspect_elf

def elf(machine=40, flags=0x05000200, types=()):
    b = bytearray(52 + 32 * len(types))
    b[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHI', b, 16, 2, machine, 1)
    struct.pack_into('<I', b, 28, 52)
    struct.pack_into('<I', b, 36, flags)
    struct.pack_into('<HHH', b, 40, 52, 32, len(types))
    for i, kind in enumerate(types): struct.pack_into('<I', b, 52 + i * 32, kind)
    return bytes(b)

class ArtifactTests(unittest.TestCase):
    def test_static_arm_softfloat(self):
        self.assertEqual(inspect_elf(elf())['abi'], 'ARM EABI5 soft-float')
    def test_reject_dynamic_or_loader(self):
        for kind in (2, 3):
            with self.subTest(kind=kind), self.assertRaises(ValueError): inspect_elf(elf(types=(kind,)))
    def test_reject_wrong_machine_or_float_abi(self):
        for b in (elf(machine=62), elf(flags=0x05000400), elf(flags=0x04000200)):
            with self.assertRaises(ValueError): inspect_elf(b)
    def test_reject_truncated_program_table(self):
        with self.assertRaises(ValueError): inspect_elf(elf(types=(1,))[:-1])

if __name__ == '__main__': unittest.main()
