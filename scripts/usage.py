#!/usr/bin/python3
"""Execute only the installed usage version pinned by this repository."""

import os
from pathlib import Path
import sys
import tomllib

ROOT = Path(__file__).resolve().parent.parent


def installed_usage(environment):
    version = tomllib.loads((ROOT / "mise.toml").read_text())["tools"]["usage"]
    home = Path(environment.get("HOME", Path.home()))
    data = Path(environment.get("MISE_DATA_DIR", Path(environment.get(
        "XDG_DATA_HOME", home / ".local/share")) / "mise"))
    return data / "installs" / "usage" / version / "usage"


def main():
    if not os.environ.get("OMAFILES_TEST_ENV"):
        os.execv(sys.executable, [sys.executable, str(ROOT / "scripts/test-environment.py"),
                                 sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]])
    usage = installed_usage(os.environ)
    if not usage.is_file() or not os.access(usage, os.X_OK):
        sys.exit(f"usage: install usage@{usage.parent.name} before validation. "
                 "Validation never installs tools.")
    os.execv(str(usage), [str(usage), *sys.argv[1:]])


if __name__ == "__main__":
    main()
