#!/usr/bin/env python3
"""Generate every qmake project from scratch and fail on any qmake warning."""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = [ROOT / "omafiles.pro", *sorted((ROOT / "tests").glob("*.pro"))]


class QmakeWarningsTest(unittest.TestCase):
    def test_fresh_qmake_prints_no_warning(self):
        for project in PROJECTS:
            with self.subTest(project=project.relative_to(ROOT)), \
                    tempfile.TemporaryDirectory(prefix="omafiles-qmake-", dir="/tmp") as build:
                result = subprocess.run(
                    ["/usr/bin/python3", str(ROOT / "scripts/test-environment.py"),
                     "qmake6", "-r", str(project)],
                    cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, result.stdout)
                warnings = [line for line in result.stdout.splitlines() if "WARNING:" in line]
                self.assertEqual(warnings, [], result.stdout)


if __name__ == "__main__":
    unittest.main()
