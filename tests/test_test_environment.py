"""Validation entry points isolate caller-owned desktop and toolchain state."""

import json
import os
from pathlib import Path
import select
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "scripts/test-environment.py"
DIRECTORIES = ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
               "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "XDG_DATA_DIRS",
               "XDG_CONFIG_DIRS", "TMPDIR", "CARGO_HOME", "RUSTUP_HOME",
               "MISE_DATA_DIR", "MISE_CONFIG_DIR", "MISE_CACHE_DIR",
               "MISE_STATE_DIR", "MISE_SYSTEM_CONFIG_DIR")
BUS_PROBE = '''
import json, os, signal, sys
from gi.repository import Gio
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
def call(method, parameters=None):
    return bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                         "org.freedesktop.DBus", method, parameters, None,
                         Gio.DBusCallFlags.NONE, 30000, None).unpack()[0]
from gi.repository import GLib
result = dict(base=os.environ["OMAFILES_TEST_ENV"],
              address=os.environ["DBUS_SESSION_BUS_ADDRESS"],
              id=call("GetId"), activatable=call("ListActivatableNames"),
              bus_pid=call("GetConnectionUnixProcessID", GLib.Variant("(s)", ("org.freedesktop.DBus",))),
              child_pid=os.getpid())
print(json.dumps(result), flush=True)
'''


class TestEnvironment(unittest.TestCase):
    def assert_process_stopped(self, pid):
        deadline = time.monotonic() + 30
        while True:
            try:
                state = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0]
            except FileNotFoundError:
                return
            if state == "Z":
                return
            self.assertLess(time.monotonic(), deadline, "owned process must stop")
            time.sleep(0.01)

    def test_provides_a_fresh_private_bus_without_inherited_desktop_services(self):
        identities = set()
        for address in (None, "", "unix:path=/tmp/omafiles-caller-bus-does-not-exist"):
            for status in (0, 23):
                with self.subTest(address=address, status=status):
                    env = os.environ.copy()
                    env.pop("DBUS_SESSION_BUS_ADDRESS", None)
                    if address is not None:
                        env["DBUS_SESSION_BUS_ADDRESS"] = address
                    result = subprocess.run(
                        ["/usr/bin/python3", str(RUNNER), "/usr/bin/python3", "-c",
                         BUS_PROBE + f"sys.exit({status})"],
                        env=env, text=True, capture_output=True, timeout=30)
                    self.assertEqual(result.returncode, status, result.stderr)
                    child = json.loads(result.stdout)
                    self.assertTrue(child["address"].startswith("unix:path=" + child["base"] + "/"))
                    self.assertEqual(child["activatable"], ["org.freedesktop.DBus"])
                    self.assertNotIn(child["id"], identities)
                    identities.add(child["id"])
                    self.assert_process_stopped(child["bus_pid"])
                    self.assertFalse(Path(child["base"]).exists())

    def test_refuses_to_execute_validation_when_the_private_bus_cannot_start(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-env-bus-", dir="/tmp") as temporary:
            fixture = Path(temporary)
            daemon = fixture / "dbus-daemon"
            daemon.write_text("#!/bin/sh\nexit 41\n")
            daemon.chmod(0o755)
            sentinel = fixture / "child-started"
            probe = "from pathlib import Path; Path(" + repr(str(sentinel)) + ").touch()"
            command = ["/usr/bin/bwrap", "--ro-bind", "/", "/", "--bind", str(ROOT), str(ROOT),
                       "--bind", "/tmp", "/tmp", "--ro-bind", str(daemon), "/usr/bin/dbus-daemon",
                       "--dev", "/dev", "--proc", "/proc", "--",
                       "/usr/bin/python3", str(RUNNER), "/usr/bin/python3", "-c", probe]
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 127, result.stderr)
            self.assertIn("dbus-run-session", result.stderr)
            self.assertFalse(sentinel.exists(), "validation requires a successfully started bus")

    def test_confines_child_writes_to_the_project_and_tmp(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-env-mount-", dir="/tmp") as temporary:
            forbidden = Path(temporary)
            sentinel = forbidden / "sentinel"
            sentinel.write_text("caller-owned")
            probe = ("from pathlib import Path; import errno; "
                     "\ntry: Path('/mnt/sentinel').write_text('changed')"
                     "\nexcept OSError as error: assert error.errno == errno.EROFS, error"
                     "\nelse: raise AssertionError('validation escaped its writable roots')\n")
            command = ["/usr/bin/bwrap", "--ro-bind", "/", "/", "--bind", str(ROOT), str(ROOT),
                       "--bind", "/tmp", "/tmp", "--bind", str(forbidden), "/mnt",
                       "--dev", "/dev", "--proc", "/proc", "--",
                       "/usr/bin/python3", str(RUNNER), "/usr/bin/python3", "-c", probe]
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sentinel.read_text(), "caller-owned")

    def test_stops_owned_descendants_after_the_main_child_exits(self):
        descendant = ("import os,signal; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                      "print(os.getpid(),flush=True); signal.pause()")
        probe = ("import os,json,subprocess; p=subprocess.Popen(['/usr/bin/python3','-c',"
                 + repr(descendant) + "],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True); "
                 "print(json.dumps([os.environ['OMAFILES_TEST_ENV'],int(p.stdout.readline())]),flush=True)")
        result = subprocess.run(["/usr/bin/python3", str(RUNNER), "/usr/bin/python3", "-c", probe],
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        base, descendant_pid = json.loads(result.stdout)
        try:
            self.assert_process_stopped(descendant_pid)
            self.assertFalse(Path(base).exists())
        finally:
            try:
                os.kill(descendant_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def test_cleans_interrupted_validation_and_propagates_child_signals(self):
        probe = BUS_PROBE + "signal.pause()"
        for recipient in ("runner", "child"):
            process = subprocess.Popen(["/usr/bin/python3", str(RUNNER), "/usr/bin/python3", "-c", probe],
                                       stdout=subprocess.PIPE, text=True)
            try:
                ready, _, _ = select.select([process.stdout], [], [], 30)
                self.assertTrue(ready, "child must report readiness before interruption")
                ready = json.loads(process.stdout.readline())
                base, child = ready["base"], ready["child_pid"]
                os.kill(process.pid if recipient == "runner" else child, signal.SIGTERM)
                self.assertEqual(process.wait(timeout=30), 143)
                self.assertFalse(Path(base).exists())
                self.assert_process_stopped(ready["bus_pid"])
                with self.assertRaises(ProcessLookupError):
                    os.kill(child, 0)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=30)
                process.stdout.close()

    def test_isolates_inherited_state_and_cleans_success_and_failure(self):
        with tempfile.TemporaryDirectory(prefix="omafiles-env-test-", dir="/tmp") as fixture:
            caller = Path(fixture)
            env = os.environ.copy()
            for key in DIRECTORIES:
                directory = caller / key
                directory.mkdir()
                (directory / "sentinel").write_text(key)
                env[key] = str(directory)
            for key in ("MISE_CONFIG_FILE", "MISE_GLOBAL_CONFIG_FILE", "MISE_SYSTEM_CONFIG_FILE"):
                env[key] = str(caller / "global.toml")
            (caller / "global.toml").write_text('[tools]\nrust = "stable"\n')
            env.update(DISPLAY=":99", WAYLAND_DISPLAY="caller-wayland",
                       DBUS_STARTER_ADDRESS="caller-bus", LD_PRELOAD="",
                       MISE_ENV="caller", XDG_ACTIVATION_TOKEN="caller-token",
                       PATH=str(caller / "caller-tools"))
            previous = None
            for status in (0, 23):
                probe = "import os,json,sys; print(json.dumps(dict(os.environ))); sys.exit(" + str(status) + ")"
                result = subprocess.run(["/usr/bin/python3", str(RUNNER), "/usr/bin/python3", "-c", probe],
                                        env=env, text=True, capture_output=True)
                self.assertEqual(result.returncode, status, result.stderr)
                child = json.loads(result.stdout)
                base = Path(child["OMAFILES_TEST_ENV"])
                self.assertEqual(base.parent, Path("/tmp"))
                self.assertNotEqual(base, previous)
                previous = base
                for key in DIRECTORIES:
                    self.assertTrue(Path(child[key]).is_relative_to(base), key)
                    self.assertEqual((caller / key / "sentinel").read_text(), key)
                for key in ("MISE_CONFIG_FILE", "MISE_GLOBAL_CONFIG_FILE", "MISE_SYSTEM_CONFIG_FILE"):
                    self.assertTrue(Path(child[key]).is_relative_to(base), key)
                for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_STARTER_ADDRESS", "LD_PRELOAD",
                            "MISE_ENV", "XDG_ACTIVATION_TOKEN"):
                    self.assertNotIn(key, child)
                for key in ("DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS", "PULSE_SERVER"):
                    self.assertIn(str(base), child[key])
                self.assertEqual(child["MISE_OFFLINE"], "true")
                self.assertEqual(child["MISE_AUTO_INSTALL"], "false")
                self.assertEqual(child["MISE_NO_CONFIG"], "1")
                self.assertEqual(child["PATH"], os.defpath)
                self.assertFalse(base.exists(), "validation scratch state must be removed")
            self.assertEqual((caller / "global.toml").read_text(), '[tools]\nrust = "stable"\n')


if __name__ == "__main__":
    unittest.main()
