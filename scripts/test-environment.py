#!/usr/bin/python3
"""Run validation with disposable desktop and toolchain state."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
from usage import installed_usage
from write_scope import confined_command


def main():
    root = Path(__file__).resolve().parent.parent
    original = os.environ.copy()
    usage = installed_usage(original)
    with tempfile.TemporaryDirectory(prefix="omafiles-test-", dir="/tmp") as temporary:
        base = Path(temporary)
        env = {key: value for key, value in original.items()
               if not key.startswith("MISE_") and key not in (
                   "DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_PID",
                   "DBUS_SESSION_BUS_ADDRESS", "DBUS_SESSION_BUS_WINDOWID",
                   "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE",
                   "LD_PRELOAD", "XDG_ACTIVATION_TOKEN")}
        for key in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                    "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "XDG_DATA_DIRS",
                    "XDG_CONFIG_DIRS", "TMPDIR", "CARGO_HOME", "RUSTUP_HOME",
                    "MISE_DATA_DIR", "MISE_CACHE_DIR", "MISE_STATE_DIR",
                    "MISE_CONFIG_DIR", "MISE_SYSTEM_CONFIG_DIR"):
            directory = base / key
            directory.mkdir(mode=0o700)
            env[key] = str(directory)
        (base / "XDG_DATA_DIRS/mime").symlink_to("/usr/share/mime", target_is_directory=True)
        config = base / "mise.toml"
        config.touch()
        env.update(PATH=os.defpath, MISE_CONFIG_FILE=str(config), MISE_GLOBAL_CONFIG_FILE=str(config),
                   MISE_SYSTEM_CONFIG_FILE=str(config), MISE_OFFLINE="true",
                   MISE_AUTO_INSTALL="false", MISE_NO_CONFIG="1",
                   MISE_CEILING_PATHS=str(root.parent), MISE_TRUSTED_CONFIG_PATHS=str(root),
                   OMAFILES_TEST_ENV=str(base), QT_QPA_PLATFORM="offscreen",
                   QT_QPA_PLATFORMTHEME="", QT_QUICK_BACKEND="software",
                   QSG_RHI_BACKEND="software", GVFS_DISABLE_FUSE="1",
                   DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(base / "no-system-bus"),
                   PULSE_SERVER="unix:" + str(base / "no-audio"),
                   PIPEWIRE_REMOTE="disconnected", OMARCHY_PATH=str(base / "omarchy"))
        alsa = base / "alsa.conf"
        alsa.write_text("pcm.!default { type null }\nctl.!default { type hw card -1 }\n")
        env["ALSA_CONFIG_PATH"] = str(alsa)
        bus_config = base / "session.conf"
        bus_config.write_text('<busconfig><type>session</type><listen>unix:path='
                              + str(base / "session-bus")
                              + '</listen><policy context="default"><allow own="*"/>'
                              + '<allow send_destination="*"/><allow receive_sender="*"/>'
                              + '</policy></busconfig>')
        if usage.is_file():
            target = Path(env["MISE_DATA_DIR"]) / "installs/usage" / usage.parent.name / "usage"
            target.parent.mkdir(parents=True)
            shutil.copy2(usage, target)
        def interrupted(number, _frame):
            raise SystemExit(128 + number)

        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, interrupted)
        command = ["/usr/bin/dbus-run-session", "--dbus-daemon=/usr/bin/dbus-daemon",
                   "--config-file=" + str(bus_config), "--", *sys.argv[1:]]
        process = subprocess.Popen(confined_command(command, [root, Path("/tmp")]),
                                   env=env, start_new_session=True)
        try:
            status = process.wait()
            return status if status >= 0 else 128 - status
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    sys.exit(main())
