#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
exec /usr/bin/python3 - "$root" "${1:?usage: refresh-portal.sh <prefix>}" <<'PY'
import os
from pathlib import Path
import shlex
import sys
import tempfile

root, prefix = map(Path, sys.argv[1:])
try:
    str(prefix).encode("utf-8")
except UnicodeEncodeError:
    sys.exit("Portal installation requires a UTF-8 prefix")
if not prefix.is_absolute() or any(character in str(prefix) for character in "\n\r\t"):
    sys.exit("Portal installation requires an absolute prefix without tabs or line breaks")


def write_file(destination, content):
    for component in (destination, *destination.parents):
        if component.is_symlink():
            sys.exit(f"Refusing to install portal registration through a symbolic link: {component}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fchmod(stream.fileno(), 0o644)
        os.replace(temporary, destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


# D-Bus reads the key-file value before parsing its command-line quoting.
command = shlex.quote(str(prefix / "bin/omafiles-studio")) + " --portal"
command = command.replace("\\", "\\\\")
service = ("[D-BUS Service]\n"
           "Name=org.freedesktop.impl.portal.desktop.omafiles\n"
           f"Exec={command}\n")
write_file(prefix / "share/dbus-1/services/org.freedesktop.impl.portal.desktop.omafiles.service",
           service.encode("utf-8"))
write_file(prefix / "share/xdg-desktop-portal/portals/omafiles.portal",
           (root / "pkgbuild/omafiles.portal").read_bytes())
PY
