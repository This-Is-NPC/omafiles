"""Documentation tooling executes only the installed project-pinned usage."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
VERSION = "6.6.0"


class UsageToolTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="omafiles-usage-test-", dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.project = self.base / "project"
        scripts = self.project / "scripts"
        scripts.mkdir(parents=True)
        for name in ("usage.py", "test-environment.py", "write_scope.py",
                     "usage-check.sh", "usage-gen.sh"):
            shutil.copy2(ROOT / "scripts" / name, scripts / name)
        (self.project / "mise.toml").write_text('[tools]\nusage = "6.6.0"\nrust = "stable"\n')
        self.env = os.environ.copy()
        self.env.pop("OMAFILES_TEST_ENV", None)
        for key in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                    "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "CARGO_HOME",
                    "RUSTUP_HOME", "MISE_DATA_DIR", "MISE_CACHE_DIR", "MISE_CONFIG_DIR",
                    "MISE_STATE_DIR", "MISE_SYSTEM_CONFIG_DIR"):
            directory = self.base / key
            directory.mkdir()
            self.env[key] = str(directory)
        poison = self.base / "global.toml"
        poison.write_text('[tools]\nrust = "stable"\nnode = "latest"\n')
        for key in ("MISE_CONFIG_FILE", "MISE_GLOBAL_CONFIG_FILE", "MISE_SYSTEM_CONFIG_FILE"):
            self.env[key] = str(poison)
        self.traps = self.base / "traps"
        self.traps.mkdir()
        for name in ("mise", "usage", "rustup", "cargo"):
            script = self.traps / name
            script.write_text('#!/bin/sh\necho unexpected-tool >&2\nexit 97\n')
            script.chmod(0o755)
        self.env["PATH"] = str(self.traps) + os.pathsep + os.defpath
        self.usage = Path(self.env["MISE_DATA_DIR"]) / "installs/usage" / VERSION / "usage"
        self.usage.parent.mkdir(parents=True)

    def run_script(self, name, *arguments):
        command = (["/usr/bin/python3"] if name.endswith(".py") else ["/usr/bin/bash"])
        confined = ["/usr/bin/bwrap", "--ro-bind", "/", "/", "--bind", "/tmp", "/tmp"]
        mise = shutil.which("mise", path=os.defpath)
        if mise:
            confined.extend(("--ro-bind", str(self.traps / "mise"), str(Path(mise).resolve())))
        confined.extend(("--dev", "/dev", "--proc", "/proc", "--"))
        return subprocess.run(confined + command + [str(self.project / "scripts" / name), *arguments],
                              env=self.env, cwd=self.project, text=True, capture_output=True,
                              timeout=30)

    def test_ignores_global_tools_and_executes_only_the_pinned_copy(self):
        self.usage.write_text('#!/usr/bin/python3\nimport os,sys,json\n'
                              'print(json.dumps({"arguments":sys.argv,"environment":dict(os.environ)}))\n')
        self.usage.chmod(0o755)
        result = self.run_script("usage.py", "lint", "file with spaces.kdl")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = json.loads(result.stdout)
        self.assertEqual(output["arguments"][1:], ["lint", "file with spaces.kdl"])
        env = output["environment"]
        copied = Path(output["arguments"][0])
        self.assertTrue(copied.is_relative_to(Path(env["MISE_DATA_DIR"])))
        self.assertEqual(copied.parts[-4:], ("installs", "usage", VERSION, "usage"))
        self.assertEqual(env["MISE_AUTO_INSTALL"], "false")
        self.assertEqual(env["MISE_NO_CONFIG"], "1")
        self.assertFalse(Path(env["OMAFILES_TEST_ENV"]).exists())
        self.assertEqual(sorted(self.usage.parent.parent.iterdir()), [self.usage.parent])

    def test_missing_pin_refuses_without_install_or_path_fallback(self):
        other = self.usage.parent.parent / "99.0.0"
        other.mkdir()
        shutil.copy2(self.traps / "usage", other / "usage")
        result = self.run_script("usage.py", "--version")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("usage@6.6.0", result.stderr)
        self.assertIn("Validation never installs tools", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(list(self.usage.parent.iterdir()), [])

    def test_check_and_generation_use_the_same_pinned_tool(self):
        import sys
        sys.path.insert(0, str(ROOT / "scripts"))
        from usage import installed_usage
        source = installed_usage(os.environ)
        shutil.copy2(source, self.usage)
        self.env["TMPDIR"] = "/outside-caller-directory-that-does-not-exist"
        (self.project / "docs").mkdir()
        for relative in ("omafiles.usage.kdl", "docs/cli.md"):
            shutil.copy2(ROOT / relative, self.project / relative)
        result = self.run_script("usage-check.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        output = self.project / "docs/cli.md"
        expected = output.read_bytes()
        output.unlink()
        result = self.run_script("usage-gen.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), expected)


if __name__ == "__main__":
    unittest.main()
