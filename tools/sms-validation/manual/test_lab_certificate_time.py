import unittest

from lab_certificate_time import choose_epoch


class LabTimeTests(unittest.TestCase):
    def test_common_valid_time_does_not_depend_on_today(self):
        self.assertEqual(choose_epoch([(100, 10000), (200, 8000), (150, 9000)]), 260)

    def test_empty_or_invalid_windows_rejected(self):
        for windows in ([], [(1, 1)], [(2, 1)], [(100, 200), (300, 400)]):
            with self.subTest(windows=windows), self.assertRaises(ValueError):
                choose_epoch(windows)

    def test_reserve_covers_full_manual_lease(self):
        with self.assertRaises(ValueError):
            choose_epoch([(100, 5560)])
        self.assertEqual(choose_epoch([(100, 5561)]), 160)


if __name__ == '__main__':
    unittest.main()
