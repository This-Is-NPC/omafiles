"""Construct validation commands with a read-only host filesystem."""

import os
from pathlib import Path
import shutil


def confined_command(command, roots):
    """Allow persistent filesystem changes only beneath the given directories."""
    executable = shutil.which("bwrap", path=os.defpath)
    if executable is None:
        raise RuntimeError("Filesystem write confinement requires bubblewrap (bwrap)")
    arguments = [executable, "--die-with-parent", "--ro-bind", "/", "/"]
    for root in roots:
        directory = Path(root).resolve(strict=True)
        if not directory.is_dir():
            raise RuntimeError(f"Filesystem write confinement root is not a directory: {directory}")
        arguments.extend(("--bind", str(directory), str(directory)))
    arguments.extend(("--dev", "/dev", "--proc", "/proc", "--"))
    return arguments + list(command)
