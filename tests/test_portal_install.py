#!/usr/bin/env python3
"""Check portal registration and activation without changing desktop settings."""

import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
SERVICE = "org.freedesktop.impl.portal.desktop.omafiles"


class PortalInstallTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="omafiles-portal-install-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.env = os.environ.copy()
        for variable in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                         "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR"):
            directory = self.root / variable
            directory.mkdir(mode=0o700)
            self.env[variable] = str(directory)
        self.env.update(DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(self.root / "no-bus"),
                        DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(self.root / "no-system-bus"),
                        PULSE_SERVER="unix:" + str(self.root / "no-audio"),
                        PIPEWIRE_REMOTE="no-pipewire", QT_QPA_PLATFORM="offscreen",
                        QT_QPA_PLATFORMTHEME="")
        self.config = Path(self.env["XDG_CONFIG_HOME"]) / "xdg-desktop-portal/hyprland-portals.conf"
        self.config.parent.mkdir()
        self.config.write_text("[preferred]\ndefault=hyprland;gtk;\norg.freedesktop.impl.portal.FileChooser=gtk\n")
        self.config_before = self.config.read_bytes()

    def install(self, prefix, success=True):
        result = subprocess.run([str(ROOT / "scripts/refresh-portal.sh"), str(prefix)],
                                env=self.env, cwd=self.root, text=True, capture_output=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        self.assertEqual(self.config.read_bytes(), self.config_before)
        return result

    def test_registration_is_idempotent_and_preserves_portal_preferences(self):
        prefix = self.root / "prefix"
        self.install(prefix)
        descriptor = prefix / "share/xdg-desktop-portal/portals/omafiles.portal"
        service = prefix / f"share/dbus-1/services/{SERVICE}.service"
        self.assertEqual(descriptor.read_bytes(), (ROOT / "pkgbuild/omafiles.portal").read_bytes())
        self.assertEqual(descriptor.read_text(), "[portal]\nDBusName=" + SERVICE
                         + "\nInterfaces=org.freedesktop.impl.portal.FileChooser;\n")
        self.assertEqual((ROOT / f"pkgbuild/{SERVICE}.service").read_text(),
                         "[D-BUS Service]\nName=" + SERVICE
                         + "\nExec=/usr/bin/omafiles-studio --portal\n")
        self.assertEqual(service.read_text(), "[D-BUS Service]\nName=" + SERVICE
                         + "\nExec=" + str(prefix / "bin/omafiles-studio") + " --portal\n")
        before = (descriptor.read_bytes(), service.read_bytes())
        self.install(prefix)
        self.assertEqual((descriptor.read_bytes(), service.read_bytes()), before)

    def test_refuses_invalid_prefixes_and_every_symbolic_link_boundary(self):
        self.install(Path("relative"), success=False)
        invalid = self.install(self.root / "invalid-\udcff", success=False)
        self.assertIn("requires a UTF-8 prefix", invalid.stderr)
        self.install(self.root / "line\nbreak", success=False)
        self.assertFalse((self.root / "line\nbreak").exists())
        for boundary in ("prefix", "share", "services", "service", "descriptor"):
            with self.subTest(boundary=boundary):
                prefix = self.root / boundary / "prefix"
                destination = self.root / boundary / "untouched"
                destination.mkdir(parents=True)
                if boundary == "prefix":
                    prefix.symlink_to(destination, target_is_directory=True)
                else:
                    self.install(prefix)
                    link = {"share": prefix / "share",
                            "services": prefix / "share/dbus-1/services",
                            "service": prefix / f"share/dbus-1/services/{SERVICE}.service",
                            "descriptor": prefix / "share/xdg-desktop-portal/portals/omafiles.portal"}[boundary]
                    if link.is_dir():
                        link.rename(link.with_name(link.name + "-original"))
                    else:
                        link.unlink()
                    link.symlink_to(destination, target_is_directory=True)
                self.install(prefix, success=False)
                self.assertEqual(list(destination.iterdir()), [])

    def test_private_bus_activates_the_exact_executable_with_reserved_characters(self):
        prefix = self.root / "prefix space 'quote' \"double\" $dollar `tick` \\slash %percent"
        executable = prefix / "bin/omafiles-studio"
        executable.parent.mkdir(parents=True)
        record = self.root / "activation.json"
        executable.write_text('''#!/usr/bin/python3
import json, os, sys
from pathlib import Path
from gi.repository import Gio, GLib
Path(os.environ["PORTAL_ACTIVATION_RECORD"]).write_text(json.dumps({"argv": sys.argv, "pid": os.getpid()}))
connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
connection.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", GLib.Variant("(su)", ("org.freedesktop.impl.portal.desktop.omafiles", 0)), None, Gio.DBusCallFlags.NONE, -1, None)
GLib.MainLoop().run()
''')
        executable.chmod(0o755)
        self.install(prefix)
        bus_config = self.root / "bus.conf"
        bus_config.write_text("<busconfig><type>session</type><listen>unix:tmpdir="
                              + escape(str(self.root)) + "</listen><servicedir>"
                              + escape(str(prefix / "share/dbus-1/services"))
                              + '</servicedir><policy context="default"><allow own="*"/>'
                              + '<allow send_destination="*"/><allow receive_sender="*"/></policy></busconfig>')
        daemon = subprocess.Popen(["dbus-daemon", "--nofork", "--print-address=1",
                                   "--config-file=" + str(bus_config)],
                                  env=dict(self.env, PORTAL_ACTIVATION_RECORD=str(record)),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            env = dict(self.env, DBUS_SESSION_BUS_ADDRESS=daemon.stdout.readline().strip(),
                       PORTAL_ACTIVATION_RECORD=str(record))
            result = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                                     "--object-path", "/org/freedesktop/DBus", "--method",
                                     "org.freedesktop.DBus.StartServiceByName", SERVICE, "0"],
                                    env=env, text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            activation = json.loads(record.read_text())
            self.assertEqual(activation["argv"], [str(executable), "--portal"])
        finally:
            if record.exists():
                os.kill(json.loads(record.read_text())["pid"], signal.SIGTERM)
            daemon.terminate()
            daemon.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
