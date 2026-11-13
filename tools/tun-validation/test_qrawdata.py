import struct
import unittest

from qrawdata_inspect import walk_cfg, find_u32_references, file_operation_callbacks


class QrawdataInspectionTests(unittest.TestCase):
    def test_only_aligned_literal_references_are_accepted(self):
        value = 0xc0008010
        data = b"x" + struct.pack("<I", value) + bytes(3) + struct.pack("<I", value)
        self.assertEqual(find_u32_references(data, value), [8])

    def test_decode_file_operations_offsets(self):
        data = bytearray(116)
        struct.pack_into("<I", data, 40, 0xc0009000)
        struct.pack_into("<I", data, 52, 0xc0009100)
        struct.pack_into("<I", data, 60, 0xc0009200)
        self.assertEqual(file_operation_callbacks(data, 0),
                         {"unlocked_ioctl": 0xc0009000, "open": 0xc0009100,
                          "release": 0xc0009200})

    def test_cfg_follows_calls_but_not_literal_pool_after_return(self):
        # bl exported_function; pop {pc}; .word that must never be decoded
        image = struct.pack("<III", 0xeb00003e, 0xe49df004, 0xffffffff)
        result = walk_cfg(image, 0x1000, [0x1000], [(0x1000, 0x100c)], {0x1100: "test_api"})
        self.assertEqual(result["external_calls"], ["test_api"])
        self.assertEqual(result["instruction_count"], 2)
        self.assertFalse(result["unresolved"])

    def test_cfg_reports_indirect_calls_instead_of_claiming_complete_coverage(self):
        result = walk_cfg(struct.pack("<II", 0xe12fff33, 0xe12fff1e), 0x1000,
                          [0x1000], [(0x1000, 0x1008)], {})
        self.assertTrue(result["unresolved"])

    def test_cfg_follows_both_conditional_paths(self):
        # bne +4; bx lr; mov r0,#0; bx lr
        image = struct.pack("<IIII", 0x1a000000, 0xe12fff1e, 0xe3a00000, 0xe12fff1e)
        result = walk_cfg(image, 0x1000, [0x1000], [(0x1000, 0x1010)], {})
        self.assertEqual(result["instruction_count"], 4)
        self.assertFalse(result["unresolved"])


if __name__ == "__main__":
    unittest.main()
