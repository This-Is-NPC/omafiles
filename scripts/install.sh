#!/usr/bin/env bash
# Build, then install the front ends, helper programs and desktop files.
# Idempotent: every install overwrites what the last one wrote.
#
# The layout is the one pkgbuilds/omafiles packages, under a prefix that
# defaults to ~/.local so a checkout install needs no root and cannot fight the
# package manager over a file. PREFIX=/usr/local installs for everyone; the
# build still runs as you and only the copies go through sudo, so nothing in
# build/ ends up owned by root.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

prefix="${PREFIX:-$HOME/.local}"

# A path nobody has created yet is writable if the nearest directory above it
# that does exist is -- which is the whole of ~/.local on a machine that has
# never installed anything there.
writable() {
    local path="$1"
    while [ ! -e "$path" ]; do
        path="$(dirname "$path")"
    done
    [ -w "$path" ]
}

as_root() {
    local target="$1"; shift
    if writable "$target"; then
        "$@"
    else
        sudo "$@"
    fi
}

"$root/scripts/build.sh" >/dev/null

install_file() {
    as_root "$3" install -Dm"$1" "$2" "$3"
}

install_file 755 build/bin/omafiles        "$prefix/bin/omafiles"
install_file 755 build/bin/omafiles-studio "$prefix/bin/omafiles-studio"
install_file 755 build/bin/omafiles-preview "$prefix/bin/omafiles-preview"
install_file 755 build/bin/omafiles-archive "$prefix/bin/omafiles-archive"
install_file 755 build/bin/omafiles-devices "$prefix/bin/omafiles-devices"
install_file 644 LICENSE                   "$prefix/share/licenses/omafiles/LICENSE"
install_file 644 pkgbuild/omafiles.svg     "$prefix/share/icons/hicolor/scalable/apps/omafiles.svg"
install_file 644 pkgbuild/omafiles.desktop "$prefix/share/applications/omafiles.desktop"

"$root/scripts/refresh-desktop.sh" "$prefix"
as_root "$prefix/share" "$root/scripts/refresh-portal.sh" "$prefix"

echo "installed $("$prefix/bin/omafiles" --version) in $prefix/bin"

# Whichever copy the shell finds first is the one that runs, and on Arch
# /usr/bin comes before ~/.local/bin in the default PATH -- so the package,
# once installed, shadows this build rather than the other way round.
found="$(command -v omafiles || true)"
if [ -z "$found" ]; then
    echo "note: $prefix/bin is not on PATH — add it to run 'omafiles' by name" >&2
elif [ "$found" != "$prefix/bin/omafiles" ]; then
    echo "note: $found comes first on PATH and is what 'omafiles' still runs" >&2
fi
