import unittest
from pathlib import Path
import tempfile

from private_audit import macro_header, compiler_arguments, sdk_references


class PrivateAuditTests(unittest.TestCase):
    def test_module_config_uses_module_macro_not_builtin_macro(self):
        text = macro_header({"CONFIG_A": "m", "CONFIG_B": "y", "CONFIG_C": "n"})
        self.assertIn("#define CONFIG_A_MODULE 1", text)
        self.assertNotIn("#define CONFIG_A 1", text)
        self.assertIn("#define CONFIG_B 1", text)
        self.assertIn("#undef CONFIG_C", text)
        self.assertNotIn("#define CONFIG_C 0", text)

    def test_rejects_unknown_config_values(self):
        with self.assertRaises(ValueError):
            macro_header({"CONFIG_A": "unexpected"})

    def test_rewrites_output_and_dependency_path_without_shell_execution(self):
        command = '/opt/gcc493/bin/arm-linux-gnueabi-gcc -Wp,-MD,old.d -D"KBUILD_STR(s)=#s" -c -o old.o /work/tun.c'
        args = compiler_arguments(command, "/work/new.o")
        self.assertNotIn("-Wp,-MD,old.d", args)
        self.assertIn("-DKBUILD_STR(s)=#s", args)
        self.assertEqual(args[args.index("-o") + 1], "/work/new.o")

    def test_scanner_detects_module_macros_in_shared_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            header = root / "include/linux/netdevice.h"
            header.parent.mkdir(parents=True)
            header.write_text("#ifdef CONFIG_QL_LPM_MODULE\nint private_field;\n#endif\n")
            references = sdk_references(root)
            self.assertEqual(len(references["CONFIG_QL_LPM"]), 1)
            self.assertEqual(references["CONFIG_QL_LPM"][0]["path"], "include/linux/netdevice.h")


if __name__ == "__main__":
    unittest.main()
