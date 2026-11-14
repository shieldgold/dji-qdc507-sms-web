"""Reject unsafe credential staging and configuration injection offline."""
import os
from pathlib import Path
import tempfile
import unittest
from prepare_connect import connection_files


class ConnectPreparationTests(unittest.TestCase):
    def test_invalid_identity_or_address_rejected_before_credentials(self):
        params = {'relay': '192.168.225.21', 'server_id': 'server.example',
                  'client_id': 'client@example', 'target': '10.1.1.1', 'vip': '10.2.2.2'}
        for key, value in [('server_id', 'server\ninclude /etc/shadow'), ('relay', '127.0.0.1'),
                           ('vip', '224.1.1.1'), ('target', '10.2.2.2')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                connection_files({**params, key: value}, Path('/missing'))

    def test_public_or_symlink_credential_directory_refused(self):
        params = {'relay': '192.168.225.21', 'server_id': 'server.example',
                  'client_id': 'client@example', 'target': '10.1.1.1', 'vip': '10.2.2.2'}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); public = root/'public'; public.mkdir(); public.chmod(0o755)
            with self.assertRaises(ValueError): connection_files(params, public)
            private = root/'private'; private.mkdir(mode=0o700)
            link = root/'link'; link.symlink_to(private, target_is_directory=True)
            with self.assertRaises(ValueError): connection_files(params, link)


if __name__ == '__main__':
    unittest.main()
