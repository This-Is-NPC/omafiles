"""Read-only mount confinement checks use disposable paths exclusively under /tmp."""

import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


MODULE = Path(__file__).resolve().parent.parent / "scripts/write_scope.py"
SPEC = importlib.util.spec_from_file_location("write_scope", MODULE)
write_scope = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(write_scope)


class WriteScope(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="omafiles-write-scope-", dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.allowed = self.base / "allowed"
        self.forbidden = self.base / "forbidden"
        self.allowed.mkdir()
        self.forbidden.mkdir()
        (self.forbidden / "existing").write_text("sentinel")
        (self.forbidden / "directory").mkdir()
        (self.allowed / "escape").symlink_to(self.forbidden, target_is_directory=True)

    def probe(self, body, inherited=False):
        imports = "import os, pathlib, subprocess, sys; from pathlib import Path; "
        operation = imports + body
        if inherited:
            operation = ("import subprocess; result = subprocess.run(['/usr/bin/python3', '-B', '-c', "
                         + repr(operation) + "]); raise SystemExit(result.returncode)")
        command = write_scope.confined_command(["/usr/bin/python3", "-B", "-c", operation],
                                               [self.allowed])
        result = subprocess.run(command,
                                text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)

    def denied(self, body, inherited=False):
        self.probe("\ntry:\n    " + body + "\nexcept OSError as error:\n"
                   "    assert error.errno in (1, 13, 18, 30), error\n"
                   "else:\n    raise AssertionError('forbidden operation succeeded')\n", inherited)
        self.assertEqual((self.forbidden / "existing").read_text(), "sentinel")

    def test_allowed_writes_and_sink(self):
        destination = repr(str(self.allowed))
        self.probe("\np = Path(" + destination + ")\n"
                   "(p / 'first').write_text('hello')\n"
                   "os.truncate(p / 'first', 2)\n"
                   "(p / 'subdir').mkdir()\n"
                   "(p / 'first').rename(p / 'subdir/renamed')\n"
                   "os.link(p / 'subdir/renamed', p / 'linked')\n"
                   "(p / 'linked').unlink()\n"
                   "(p / 'subdir/renamed').unlink()\n"
                   "(p / 'subdir').rmdir()\n"
                   "with open('/dev/null', 'w') as sink: sink.write('discard')\n")

    def test_forbidden_write(self):
        self.denied(f"open({str(self.forbidden / 'existing')!r}, 'a').write('bad')")

    def test_forbidden_create(self):
        self.denied(f"Path({str(self.forbidden / 'new')!r}).write_text('bad')")

    def test_forbidden_truncate(self):
        self.denied(f"os.truncate({str(self.forbidden / 'existing')!r}, 0)")

    def test_forbidden_chmod(self):
        self.denied(f"os.chmod({str(self.forbidden / 'existing')!r}, 0o600)")

    def test_forbidden_utime(self):
        self.denied(f"os.utime({str(self.forbidden / 'existing')!r}, (1, 1))")

    def test_forbidden_link(self):
        self.denied(f"os.link({str(self.forbidden / 'existing')!r}, {str(self.forbidden / 'linked')!r})")
        self.denied(f"os.link({str(self.forbidden / 'existing')!r}, {str(self.allowed / 'linked')!r})")

    def test_forbidden_remove(self):
        self.denied(f"os.unlink({str(self.forbidden / 'existing')!r})")

    def test_forbidden_mkdir(self):
        self.denied(f"os.mkdir({str(self.forbidden / 'newdir')!r})")

    def test_forbidden_rmdir(self):
        self.denied(f"os.rmdir({str(self.forbidden / 'directory')!r})")

    def test_forbidden_symlink_creation(self):
        self.denied(f"os.symlink('target', {str(self.forbidden / 'link')!r})")

    def test_forbidden_fifo_creation(self):
        self.denied(f"os.mkfifo({str(self.forbidden / 'fifo')!r})")

    def test_forbidden_rename(self):
        (self.allowed / "source").write_text("source")
        self.denied(f"os.rename({str(self.forbidden / 'existing')!r}, {str(self.forbidden / 'renamed')!r})")
        self.denied(f"os.rename({str(self.allowed / 'source')!r}, {str(self.forbidden / 'new')!r})")
        self.denied(f"os.rename({str(self.forbidden / 'existing')!r}, {str(self.allowed / 'new')!r})")

    def test_symlink_cannot_escape(self):
        for filename in ("existing", "new"):
            with self.subTest(filename=filename):
                self.denied(f"Path({str(self.allowed / 'escape' / filename)!r}).write_text('bad')")

    def test_child_inherits_restriction(self):
        self.denied(f"Path({str(self.forbidden / 'new')!r}).write_text('bad')", inherited=True)

    def test_read_access_is_unrestricted(self):
        self.probe(f"assert Path({str(self.forbidden / 'existing')!r}).read_text() == 'sentinel'")

    def test_missing_bubblewrap_fails_closed(self):
        with patch.object(write_scope.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "requires bubblewrap"):
                write_scope.confined_command(["/usr/bin/true"], [self.allowed])

    def test_unusable_namespace_fails_closed(self):
        with patch.object(write_scope.shutil, "which", return_value="/usr/bin/false"):
            marker = self.allowed / "marker"
            command = write_scope.confined_command(
                ["/usr/bin/python3", "-c", f"open({str(marker)!r}, 'w').close()"], [self.allowed])
            result = subprocess.run(command, capture_output=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
