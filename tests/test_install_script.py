#!/usr/bin/env python3
"""install.sh downloads a release, checks it against SHA256SUMS and hands it to pacman."""

import hashlib
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = "omafiles-0.2.0-1-x86_64.pkg.tar.zst"


class InstallScriptTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(dir="/tmp")
        root = Path(self.scratch.name)
        self.release = root / "release"
        self.release.mkdir()
        bin_dir = root / "bin"
        bin_dir.mkdir()
        # install.sh only asks whether pacman exists before printing the command.
        pacman = bin_dir / "pacman"
        pacman.write_text("#!/bin/sh\nexit 0\n")
        pacman.chmod(0o755)
        self.env = {"PATH": f"{bin_dir}:/usr/bin:/bin", "HOME": str(root), "TMPDIR": str(root),
                    "BASE_URL": f"file://{self.release}", "DRY_RUN": "1"}

    def tearDown(self):
        self.scratch.cleanup()

    def publish(self, content, listed=PACKAGE, digest=None):
        (self.release / PACKAGE).write_bytes(content)
        digest = digest or hashlib.sha256(content).hexdigest()
        (self.release / "SHA256SUMS").write_text(f"{digest}  {listed}\n")

    def install(self):
        return subprocess.run(["bash", str(ROOT / "install.sh")], env=self.env,
                              capture_output=True, text=True, stdin=subprocess.DEVNULL)

    def test_a_matching_package_is_handed_to_pacman(self):
        self.publish(b"package")
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout, rf"^sudo pacman -U \S+/{PACKAGE}\n$")
        self.assertIn(f"downloading {PACKAGE}", result.stderr)

    def test_a_package_that_does_not_match_its_checksum_is_refused(self):
        self.publish(b"package", digest="0" * 64)
        result = self.install()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"{PACKAGE} does not match the release's SHA256SUMS; nothing was installed.",
                      result.stderr)
        self.assertNotIn("pacman", result.stdout)

    def test_a_checksum_list_without_a_package_is_refused(self):
        self.publish(b"package", listed="something-else.tar.gz")
        result = self.install()
        self.assertEqual(result.returncode, 1)
        self.assertIn("the release's SHA256SUMS names no omafiles package.", result.stderr)


if __name__ == "__main__":
    unittest.main()
