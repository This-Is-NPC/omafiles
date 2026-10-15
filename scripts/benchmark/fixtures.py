#!/usr/bin/python3
"""Populate benchmark fixtures exclusively in a dedicated Docker volume."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import zlib

FIXTURES = ("1000", "10000", "100000", "1000000", "images-2000")


def png(index):
    def chunk(kind, data):
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
    pixels = b"".join(b"\0" + bytes(((index + row) % 256, index % 256, row % 256)) * 256 for row in range(256))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack("!2I5B", 256, 256, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def populate(fixture):
    root = Path("/fixtures")
    if root.is_symlink() or not root.is_mount():
        raise RuntimeError("/fixtures must be a dedicated mounted Docker volume")
    destination = root / fixture
    if destination.is_symlink():
        raise RuntimeError("A fixture directory cannot be a symbolic link")
    destination.mkdir(mode=0o755, exist_ok=True)
    images = fixture == "images-2000"
    expected = 2000 if images else int(fixture)
    for index in range(expected):
        name = f"image-{index:06}.png" if images else f"file-{index:06}.txt"
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW
        with os.fdopen(os.open(destination / name, flags, 0o644), "wb") as stream:
            if images:
                stream.write(png(index))
    actual = 0
    with os.scandir(destination) as entries:
        for entry in entries:
            if not entry.is_file(follow_symlinks=False):
                raise RuntimeError(f"Unexpected non-file in fixture {fixture}: {entry.name}")
            actual += 1
    if actual != expected:
        raise RuntimeError(f"Fixture {fixture} has {actual} files; expected {expected}")
    result = {"file_count": actual, "expected_count": expected}
    if images:
        result.update(width=256, height=256, format="PNG", color="RGB", bit_depth=8,
                      first_sha256=hashlib.sha256((destination / "image-000000.png").read_bytes()).hexdigest(),
                      last_sha256=hashlib.sha256((destination / "image-001999.png").read_bytes()).hexdigest())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", action="append", choices=FIXTURES)
    parser.add_argument("--all", action="store_true")
    arguments = parser.parse_args()
    if not arguments.all and not arguments.fixture:
        parser.error("Select --fixture or --all")
    manifest = {fixture: populate(fixture) for fixture in (FIXTURES if arguments.all else dict.fromkeys(arguments.fixture))}
    print(json.dumps({"schema_version": 1, "fixtures": manifest}, separators=(",", ":")))


if __name__ == "__main__":
    main()
