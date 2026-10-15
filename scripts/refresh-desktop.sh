#!/usr/bin/env bash
# Rebuild the desktop and icon caches under a prefix -- what the package's
# post_install hook does, so the launcher and its icon appear in the menu, and
# disappear from it, without a logout. Both tools are optional and a cache that
# will not rebuild is not worth failing an install or a removal over.
set -euo pipefail

prefix="${1:?usage: refresh-desktop.sh <prefix>}"

# refresh <dir the cache lives in> <command...>
refresh() {
    local dir="$1"; shift
    command -v "$1" >/dev/null 2>&1 || return 0
    [ -d "$dir" ] || return 0
    if [ -w "$dir" ]; then
        "$@" || true
    else
        sudo "$@" || true
    fi
}

apps="$prefix/share/applications"
icons="$prefix/share/icons/hicolor"
refresh "$apps" update-desktop-database -q "$apps"
refresh "$icons" gtk-update-icon-cache -q -t -f "$icons"
