import struct
import unittest

from inspect_tun import Elf32, exported_symbols, compare_modules, config_values, config_differences


def elf_fixture(vermagic="3.18.44 preempt mod_unload ARMv7 p2v8 ", module_size=328,
                imports=("misc_register",)):
    names = ["", ".shstrtab", ".modinfo", ".gnu.linkonce.this_module", ".strtab", ".symtab"]
    shstr = b"\0"
    name_offsets = []
    for name in names:
        name_offsets.append(len(shstr))
        shstr += name.encode() + b"\0"
    strings = b"\0"
    symbols = bytes(16)
    for name in imports:
        symbols += struct.pack("<IIIBBH", len(strings), 0, 0, 0x10, 0, 0)
        strings += name.encode() + b"\0"
    payloads = [b"", shstr, b"vermagic=" + vermagic.encode() + b"\0",
                bytes(module_size), strings, symbols]
    content = bytearray(52)
    sections = []
    for index, data in enumerate(payloads):
        offset = len(content)
        content.extend(data)
        sections.append((name_offsets[index], 2 if index == 5 else 1, 0, 0,
                         offset, len(data), 4 if index == 5 else 0, 0, 1,
                         16 if index == 5 else 0))
    section_offset = len(content)
    for section in sections:
        content.extend(struct.pack("<10I", *section))
    struct.pack_into("<16sHHIIIIIHHHHHH", content, 0,
                     b"\x7fELF\x01\x01\x01" + bytes(9), 1, 40, 1, 0, 0,
                     section_offset, 0x5000000, 52, 0, 0, 40, len(sections), 1)
    return bytes(content)


class InspectionTests(unittest.TestCase):
    def test_accepts_matching_metadata_and_resolved_imports(self):
        module = Elf32(elf_fixture())
        result = compare_modules(module, module, {"misc_register"})
        self.assertEqual(result["errors"], [])
        self.assertEqual(result["runtime_verified"], False)

    def test_rejects_same_kernel_version_with_wrong_preempt_flag(self):
        candidate = Elf32(elf_fixture(vermagic="3.18.44 mod_unload ARMv7 p2v8 "))
        self.assertIn("vermagic mismatch", compare_modules(
            candidate, Elf32(elf_fixture()), {"misc_register"})["errors"])

    def test_rejects_missing_kernel_export(self):
        module = Elf32(elf_fixture(imports=("not_exported",)))
        self.assertEqual(compare_modules(module, module, set())["missing_exports"],
                         ["not_exported"])

    def test_rejects_module_structure_size_difference(self):
        result = compare_modules(Elf32(elf_fixture(module_size=332)),
                                 Elf32(elf_fixture()), {"misc_register"})
        self.assertIn("struct module size mismatch", result["errors"])

    def test_rejects_non_arm_and_truncated_elf(self):
        bad = bytearray(elf_fixture())
        struct.pack_into("<H", bad, 18, 62)
        with self.assertRaises(ValueError):
            Elf32(bytes(bad))
        with self.assertRaises(ValueError):
            Elf32(elf_fixture()[:80])

    def test_export_table_accepts_four_byte_offset_and_rejects_short_noise(self):
        base = 0xc0008000
        image = bytearray(4096)
        for index in range(130):
            name_offset = 2048 + index * 12
            name = ("symbol_%03d" % index).encode() + b"\0"
            image[name_offset:name_offset + len(name)] = name
            struct.pack_into("<II", image, 4 + index * 8,
                             base + 1800, base + name_offset)
        exports = exported_symbols(bytes(image), base, minimum_run=128)
        self.assertEqual(len(exports), 130)
        self.assertIn("symbol_129", exports)
        self.assertEqual(exported_symbols(bytes(image[:1024]), base), {})

    def test_disabled_config_is_not_confused_with_missing_key(self):
        values = config_values("# CONFIG_TUN is not set\nCONFIG_NET=y\n")
        self.assertEqual(values["CONFIG_TUN"], "n")
        self.assertNotIn("CONFIG_UNKNOWN", values)

    def test_rejects_unexpected_config_change(self):
        target = {"CONFIG_TUN": "n", "CONFIG_SMP": "n", "CONFIG_QUECTEL_DRIVER": "y"}
        built = {"CONFIG_TUN": "m", "CONFIG_SMP": "y"}
        differences, unexpected = config_differences(target, built)
        self.assertEqual(len(differences), 3)
        self.assertEqual([item["key"] for item in unexpected], ["CONFIG_SMP"])

    def test_private_gap_allowance_does_not_allow_changing_its_value(self):
        _, unexpected = config_differences({"CONFIG_QUECTEL_DRIVER": "y"},
                                           {"CONFIG_QUECTEL_DRIVER": "n"})
        self.assertEqual(len(unexpected), 1)

    def test_rejects_different_arm_abi_flags(self):
        candidate = bytearray(elf_fixture())
        struct.pack_into("<I", candidate, 36, 0x5000400)
        result = compare_modules(Elf32(bytes(candidate)), Elf32(elf_fixture()), {"misc_register"})
        self.assertIn("ARM ELF ABI flags mismatch", result["errors"])


if __name__ == "__main__":
    unittest.main()
