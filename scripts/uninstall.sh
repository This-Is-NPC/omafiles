#!/usr/bin/env bash
# Remove what install.sh wrote, from the same prefix it wrote to. Idempotent:
# a file that is already gone is one less thing to remove.
set -euo pipefail

prefix="${PREFIX:-$HOME/.local}"

writable() {
    local path="$1"
    while [ ! -e "$path" ]; do
        path="$(dirname "$path")"
    done
    [ -w "$path" ]
}

remove() {
    [ -e "$1" ] || return 0
    if writable "$1"; then rm -f "$1"; else sudo rm -f "$1"; fi
    echo "removed $1"
}

remove "$prefix/bin/omafiles"
remove "$prefix/bin/omafiles-studio"
remove "$prefix/share/xdg-desktop-portal/portals/omafiles.portal"
remove "$prefix/share/dbus-1/services/org.freedesktop.impl.portal.desktop.omafiles.service"
remove "$prefix/bin/omafiles-archive"
remove "$prefix/bin/omafiles-devices"
remove "$prefix/share/licenses/omafiles/LICENSE"
remove "$prefix/share/icons/hicolor/scalable/apps/omafiles.svg"
remove "$prefix/share/applications/omafiles.desktop"
rmdir "$prefix/share/licenses/omafiles" 2>/dev/null || true

"$(cd "$(dirname "$0")" && pwd)/refresh-desktop.sh" "$prefix"

# The index is the user's data and outlives the program, the way a package
# leaves ~/.config alone. Said out loud so it is a choice and not a leftover.
index="${XDG_DATA_HOME:-$HOME/.local/share}/omafiles/index.jsonl"
if [ -e "$index" ]; then
    echo "kept $index — the tag index is yours to delete"
fi
