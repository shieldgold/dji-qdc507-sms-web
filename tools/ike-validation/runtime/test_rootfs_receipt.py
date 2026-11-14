"""Validate the actual program embedded in a lab initrd, without extraction."""
import gzip
import hashlib
from pathlib import Path
import tempfile
import unittest
import prepare_coexist


def archive(files):
    data = bytearray()
    for name, body in [*files, ('TRAILER!!!', b'')]:
        encoded = name.encode()+b'\0'
        fields = [1, 0o100500, 0, 0, 1, 0, len(body), 0, 0, 0, 0, len(encoded), 0]
        data.extend(b'070701'+b''.join(f'{n:08x}'.encode() for n in fields))
        data.extend(encoded); data.extend(b'\0'*(-len(data)%4))
        data.extend(body); data.extend(b'\0'*(-len(data)%4))
    return gzip.compress(data)


class RootfsReceiptTests(unittest.TestCase):
    def test_receipt_checks_embedded_guard_instead_of_sibling_only(self):
        self.assertTrue(hasattr(prepare_coexist, 'rootfs_guard_hash'), 'embedded guard verification is missing')
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'initrd.gz'; path.write_bytes(archive([('./bin/qdc-ike-guard', b'old-guard')]))
            actual = prepare_coexist.rootfs_guard_hash(path)
            self.assertEqual(actual, hashlib.sha256(b'old-guard').hexdigest())
            self.assertNotEqual(actual, hashlib.sha256(b'new-guard').hexdigest())

    def test_duplicate_missing_and_truncated_embedded_guard_are_rejected(self):
        self.assertTrue(hasattr(prepare_coexist, 'rootfs_guard_hash'), 'embedded guard verification is missing')
        with tempfile.TemporaryDirectory() as td:
            path = Path(td)/'initrd.gz'
            for content in [archive([]), archive([('bin/qdc-ike-guard', b'a'), ('./bin/qdc-ike-guard', b'b')]),
                            gzip.compress(b'070701')]:
                path.write_bytes(content)
                with self.assertRaises(ValueError): prepare_coexist.rootfs_guard_hash(path)


if __name__ == '__main__': unittest.main()
