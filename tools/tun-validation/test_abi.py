import struct
import unittest

from abi_access import walk_accesses
from tun_abi_inspect import normalize_words, branch_target, prel31, project_fields


def arm(*words):
    return struct.pack('<%dI' % len(words), *words)


class AbiRelocationTests(unittest.TestCase):
    def test_link_branch_address_can_change(self):
        self.assertEqual(normalize_words(arm(0xebfffffe), arm(0xeb001234), [{'offset': 0, 'type': 28}]), [])

    def test_field_displacement_is_never_masked(self):
        self.assertEqual(normalize_words(arm(0xe5901054), arm(0xe5901058), []), [0])

    def test_call_opcode_difference_is_preserved(self):
        self.assertEqual(normalize_words(arm(0xebfffffe), arm(0xea001234), [{'offset': 0, 'type': 28}]), [0])

    def test_unknown_and_unaligned_relocations_fail(self):
        for relocation in [{'offset': 0, 'type': 99}, {'offset': 1, 'type': 2}, {'offset': 4, 'type': 2}]:
            with self.assertRaises(ValueError):
                normalize_words(arm(0), arm(0), [relocation])

    def test_branch_negative_displacement_and_high_address(self):
        self.assertEqual(branch_target(0xeafffffe, 0xc0008000), 0xc0008000)
        self.assertEqual(branch_target(0xeb000000, 0xc0008000), 0xc0008008)
        with self.assertRaises(ValueError):
            branch_target(0xe5900000, 0xc0008000)

    def test_prel31_is_signed_31_bit(self):
        self.assertEqual(prel31(0x7ffffff0, 0xc05a0000), 0xc059fff0)
        self.assertEqual(prel31(0x10, 0xc05a0000), 0xc05a0010)


class AbiAccessTests(unittest.TestCase):
    def test_caller_saved_origin_is_dropped_callee_saved_is_retained(self):
        code = arm(0xe1a04000, 0xeb000000, 0xe5941054, 0xe5902058, 0xe12fff1e)
        accesses, unknown, _ = walk_accesses(code, 0x1000, 0x1000, 0x1000 + len(code))
        self.assertIn(('arg0', 84, 'ldr'), accesses)
        self.assertNotIn(('arg0', 88, 'ldr'), accesses)
        self.assertFalse(unknown)

    def test_signed_high_address_branch_reaches_both_paths(self):
        code = arm(0x1a000001, 0xe5901054, 0xe12fff1e, 0xe5901058, 0xe12fff1e)
        accesses, unknown, _ = walk_accesses(code, 0xc0008000, 0xc0008000, 0xc0008000 + len(code))
        self.assertIn(('arg0', 84, 'ldr'), accesses)
        self.assertIn(('arg0', 88, 'ldr'), accesses)
        self.assertFalse(unknown)

    def test_pointer_field_path_and_load_width_are_preserved(self):
        code = arm(0xe590109c, 0xe5d12001, 0xe12fff1e)
        accesses, _, _ = walk_accesses(code, 0, 0, len(code))
        self.assertIn(('arg0[156]', 1, 'ldrb'), accesses)
        self.assertNotIn(('arg0[156]', 1, 'ldr'), accesses)

    def test_shifted_constant_in_address_is_scaled(self):
        # mov r1,#1; add r2,r0,r1,lsl#3; ldr r3,[r2,#4]; bx lr
        code = arm(0xe3a01001, 0xe0802181, 0xe5923004, 0xe12fff1e)
        accesses, _, _ = walk_accesses(code, 0, 0, len(code))
        self.assertIn(('arg0', 12, 'ldr'), accesses)

    def test_conditional_pointer_overwrite_does_not_survive_join(self):
        code = arm(0x03a00000, 0xe5901054, 0xe12fff1e)
        accesses, _, _ = walk_accesses(code, 0, 0, len(code))
        self.assertFalse(accesses)

    def test_bounded_switch_excludes_table_from_instructions(self):
        base = 0x1000
        code = arm(0xe3530001, 0x979ff103, 0xe12fff1e, base + 20, base + 28,
                   0xe5901054, 0xe12fff1e, 0xe5901058, 0xe12fff1e)
        accesses, unknown, switches = walk_accesses(code, base, base, base + len(code))
        self.assertIn(('arg0', 84, 'ldr'), accesses)
        self.assertIn(('arg0', 88, 'ldr'), accesses)
        self.assertEqual(switches, [base + 4])
        self.assertFalse(unknown)

    def test_unbounded_indirect_branch_stays_unknown(self):
        code = arm(0xe12fff13)  # bx r3, not a return
        _, unknown, _ = walk_accesses(code, 0, 0, len(code))
        self.assertEqual(unknown, [(0, 'computed PC write')])


class AbiFieldProjectionTests(unittest.TestCase):
    def test_bitfield_storage_word_is_not_confused_with_its_byte(self):
        layouts = {'sock': {'fields': {'shutdown': {'offset': 196, 'lsb': 0, 'byte_size': 4},
                                     'type': {'offset': 196, 'lsb': 16, 'byte_size': 4},
                                     'lock': {'offset': 196, 'byte_size': 0}}}}
        self.assertEqual(project_fields('arg0', 196, {'arg0': 'sock'}, layouts), ('sock', ['shutdown']))
        self.assertEqual(project_fields('arg0', 198, {'arg0': 'sock'}, layouts), ('sock', ['type']))

    def test_nested_pointer_field_projection(self):
        layouts = {'net_device': {'fields': {'netdev_ops': {'offset': 272, 'pointer_to': 'net_device_ops'}}},
                   'net_device_ops': {'fields': {'ndo_open': {'offset': 8}}}}
        self.assertEqual(project_fields('arg0[272]', 8, {'arg0': 'net_device'}, layouts),
                         ('net_device_ops', ['ndo_open']))

    def test_ambiguous_pointee_is_not_attributed(self):
        layouts = {'union': {'fields': {'left': {'offset': 0, 'pointer_to': 'A'}, 'right': {'offset': 0, 'pointer_to': 'B'}}},
                   'A': {'fields': {}}, 'B': {'fields': {}}}
        self.assertEqual(project_fields('arg0[0]', 4, {'arg0': 'union'}, layouts), (None, []))


if __name__ == '__main__':
    unittest.main()
