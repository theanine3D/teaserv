"""Unit tests for the Accounts window's logic (no window, no server).

    python -m unittest discover -s tools/admin-gui/tests -p "test_*.py"
"""

import os
import random
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from teaserv_admin import accounts_window as aw  # noqa: E402

# An account file as the server writes it (phosg JSON with hexadecimal integers).
ACCOUNT = """{
  "AccountID": 0x1A2B3C4D,
  "AutoPatchesEnabled": [],
  "AutoReplyMessage": "",
  "BBLicenses": [{"Password": "secret", "UserName": "alice"}],
  "BBTeamID": 0x00000000,
  "BanEndTime": %s,
  "DCLicenses": [],
  "DCNTELicenses": [],
  "Ep3CurrentMeseta": 0x00000064,
  "Ep3TotalMesetaEarned": 0x00000064,
  "Flags": %s,
  "FormatVersion": 1,
  "GCLicenses": [{"AccessKey": "123456789012", "Password": "pw", "SerialNumber": 0x18F6D0E1}],
  "LastPlayerName": "Alice",
  "PCLicenses": [],
  "UserFlags": 0x00000000,
  "XBLicenses": []
}
"""


class AccountsTest(unittest.TestCase):
    def test_roles(self):
        self.assertEqual(aw.role_of(0), "Player")
        self.assertEqual(aw.role_of(0x00000040), "Player")          # FREE_JOIN_GAMES alone
        self.assertEqual(aw.role_of(0x00000007), "Moderator")
        self.assertEqual(aw.role_of(0x00000017), "Moderator")       # plus CHANGE_EVENT
        self.assertEqual(aw.role_of(0x000000FF), "Administrator")
        self.assertEqual(aw.role_of(0x7FFFFFFF), "Root")
        self.assertEqual(aw.role_of(0xFFFFFFFF), "Root")
        self.assertEqual(aw.flags_for("Moderator"), 0x7)
        self.assertEqual(aw.flags_for("Administrator", 0x80000007), 0x800000FF)   # the shared flag stays
        self.assertEqual(aw.flags_for("Player", 0x7FFFFFFF), 0)
        with self.assertRaises(ValueError):
            aw.flags_for("Owner")

    def test_read_accounts(self):
        with tempfile.TemporaryDirectory() as root:
            folder = os.path.join(root, "system", "licenses")
            os.makedirs(folder)
            future = int((time.time() + 86400) * 1000000)
            with open(os.path.join(folder, "0439041101.json"), "w", encoding="utf-8") as f:
                f.write(ACCOUNT % ("0x%016X" % future, "0x000000FF"))
            with open(os.path.join(folder, "0000000005.json"), "w", encoding="utf-8") as f:
                f.write('{"AccountID": 5, "Flags": 0, "GCLicenses": [], "BanEndTime": 1}')
            with open(os.path.join(folder, "broken.json"), "w", encoding="utf-8") as f:
                f.write("{not json")
            accounts = aw.read_accounts(root)
            self.assertEqual([a["id"] for a in accounts], [5, 0x1A2B3C4D, None])
            alice = accounts[1]
            self.assertEqual((alice["name"], alice["role"]), ("Alice", "Administrator"))
            self.assertEqual(alice["logins"], "GC 418828513, BB alice")
            self.assertNotIn("secret", repr(alice))
            self.assertNotIn("123456789012", repr(alice))
            self.assertEqual(alice["banned_until"], future)
            self.assertIsNone(accounts[0]["banned_until"])               # a ban that ended
            self.assertEqual(accounts[0]["logins"], "(no login yet)")
            self.assertTrue(accounts[2]["logins"].startswith("unreadable"))
        self.assertEqual(aw.read_accounts(os.path.join(root, "missing")), [])

    def test_checks(self):
        accounts = [{"id": 7, "logins": "GC 418828513, BB alice"}]
        self.assertEqual(aw.check_gc("0418572923", "282949185033", "hunter2"), "")
        self.assertIn("serial", aw.check_gc("12345678901", "282949185033", "x"))
        self.assertIn("serial", aw.check_gc("abc", "282949185033", "x"))
        self.assertIn("access key", aw.check_gc("1", "28294918503", "x"))
        self.assertIn("password", aw.check_gc("1", "282949185033", "toolongpw"))
        self.assertIn("password", aw.check_gc("1", "282949185033", "a b"))
        self.assertIn("00000007", aw.check_gc("418828513", "282949185033", "pw", accounts))
        self.assertEqual(aw.check_bb("bob", "trustno1", accounts), "")
        self.assertIn("00000007", aw.check_bb("alice", "pw", accounts))
        self.assertIn("user name", aw.check_bb("a b", "pw"))
        self.assertIn("password", aw.check_bb("bob", ""))

    def test_commands(self):
        commands = aw.register_commands(0x1234ABCD, "Moderator", "GC", ["0418572923", "282949185033", "hunter2"])
        self.assertEqual([c for c, _ in commands],
                         ["add-account id=1234ABCD flags=7", "add-license 1234ABCD GC 418572923 282949185033 hunter2"])
        shown = " ".join(s for _, s in commands)
        self.assertNotIn("hunter2", shown)
        self.assertNotIn("282949185033", shown)
        commands = aw.register_commands(0x10, "Root", "BB", ["bob", "trustno1"])
        self.assertEqual(commands[0][0], "add-account id=00000010 flags=7FFFFFFF")
        self.assertNotIn("trustno1", commands[1][1])
        self.assertEqual(aw.role_command({"id": 0x1A2B3C4D, "flags": 0x80000000}, "Administrator"),
                         "update-account 1A2B3C4D flags=800000FF")
        ids = {aw.new_account_id([{"id": 0x10000}], random.Random(n)) for n in range(50)}
        self.assertTrue(all(0x10000 < i < 0x7FFFFFFF for i in ids))


if __name__ == "__main__":
    unittest.main()
