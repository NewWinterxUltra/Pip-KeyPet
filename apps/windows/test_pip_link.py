"""Unit tests for the Windows companion's parsing and line formatting (no Bluetooth needed).

Run: python -m unittest discover -s apps/windows -p "test_*.py"
"""
import unittest
import xml.etree.ElementTree as ET

import pip_link as p


class PayloadTest(unittest.TestCase):
    def test_title_and_text(self):
        xml = ('<toast><visual><binding template="ToastGeneric">'
               '<text>Telegram</text><text>Аня: привет</text>'
               '<text placement="attribution">web.telegram.org</text>'
               '</binding></visual></toast>').encode("utf-8")
        self.assertEqual(p.parse_payload(xml), ("Telegram", "Аня: привет"))

    def test_header_fallback(self):
        xml = b'<toast><header title="Defender"/></toast>'
        self.assertEqual(p.parse_payload(xml), ("Defender", ""))

    def test_broken_xml(self):
        self.assertEqual(p.parse_payload(b"not xml"), ("", ""))


class CleanTest(unittest.TestCase):
    def test_pipe_and_newlines(self):
        self.assertEqual(p.clean("a|b\nc\r\nd", 100), "a/b c d")

    def test_truncate(self):
        self.assertEqual(p.clean("abcdef", 3), "abc")


class AppNameTest(unittest.TestCase):
    def test_known(self):
        self.assertEqual(p.app_name("Chrome.OAGSUHC35RUL7IWT2G4GSEQFCE"), "Chrome")
        self.assertEqual(p.app_name("Microsoft.YourPhone_8wekyb3d8bbwe!App"), "Phone Link")

    def test_unknown_not_empty(self):
        self.assertTrue(p.app_name("Some.Random.App"))


class PollTest(unittest.TestCase):
    def test_new_and_gone(self):
        n = p.Notifications()
        rows = {}
        n.fetch = lambda: dict(rows)
        n.prime()  # nothing in the Action Center at start
        rows[1] = ("A", "t", "x", 10, 0)
        rows[2] = ("B", "u", "y", 11, 1)
        new, _ = n.poll()
        self.assertEqual([x[0] for x in new], [1, 2])
        del rows[1]
        rows[3] = ("C", "v", "z", 12, 2)
        new, gone = n.poll()
        self.assertEqual([x[0] for x in new], [3])
        self.assertEqual(gone, [1])


if __name__ == "__main__":
    unittest.main()
