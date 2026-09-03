#!/usr/bin/env bash
# Build and open the studio window.
#
# Most of this file is about finding a display, because that is what actually
# stops the window opening. Qt's own failure for a missing one is eight lines
# about plugins that were found and could not be loaded followed by SIGABRT,
# which reads like a broken build and is almost always a shell that never had
# the graphical environment in it: a bare tty, an ssh session, a service unit,
# or -- by far the most common -- a multiplexer pane older than the session it
# is now sitting in.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

socket_for() {
    case "$1" in
        /*) printf '%s' "$1" ;;
        *) printf '%s/%s' "$runtime" "$1" ;;
    esac
}

usable_display() {
    if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -S "$(socket_for "$WAYLAND_DISPLAY")" ]; then
        return 0
    fi
    [ -n "${DISPLAY:-}" ]
}

# A multiplexer pane keeps the environment it was born with, and the server it
# belongs to was usually started from a session that did have a display -- so
# the answer is one query away and there is no reason to make somebody paste an
# `eval` to get it.
#
# The session environment is asked first and the global one second, and the
# order matters: attaching from ssh copies the attaching shell's environment
# into the session, so a session re-attached from a terminal with no display
# holds `-WAYLAND_DISPLAY`, an explicit "there is none" that shadows the good
# value the server still has.
#
# It says what it took and from where. A window that opens on a screen you are
# not sitting in front of is worse than one that does not open, and over ssh
# that is exactly what this recovery does.
tmux_env() {
    local line
    for scope in "" "-g"; do
        # shellcheck disable=SC2086
        line="$(tmux show-environment $scope "$1" 2>/dev/null)" || continue
        case "$line" in
            "$1"=*) printf '%s' "${line#*=}"; return 0 ;;
        esac                                     # `-NAME` means: none here
    done
    return 1
}

recover_from_tmux() {
    [ -n "${TMUX:-}" ] || return 1
    command -v tmux >/dev/null 2>&1 || return 1

    local value
    for name in WAYLAND_DISPLAY DISPLAY; do
        value="$(tmux_env "$name")" || continue
        [ -n "$value" ] || continue
        if [ "$name" = WAYLAND_DISPLAY ] && [ ! -S "$(socket_for "$value")" ]; then
            continue
        fi
        export "$name=$value"
    done

    usable_display || return 1
    echo "studio: this pane has no display; using ${WAYLAND_DISPLAY:-$DISPLAY} from the tmux session." >&2
    echo "        the window opens on that compositor's screen, which over ssh is not this one." >&2
}

no_display() {
    echo "studio: no display to open a window on." >&2
    echo "  WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-<unset>}" >&2
    echo "  XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-<unset>}" >&2
    echo "  DISPLAY=${DISPLAY:-<unset>}" >&2
    echo "$1" >&2

    # The sockets that are actually there, because the answer to "which one" is
    # on the disk and nobody should have to guess at it.
    local found=() sock
    for sock in "$runtime"/wayland-*; do
        [ -S "$sock" ] && found+=("$(basename "$sock")")
    done
    if [ ${#found[@]} -gt 0 ]; then
        echo >&2
        echo "Compositors running here: ${found[*]}" >&2
        echo "  WAYLAND_DISPLAY=${found[0]} mise run studio" >&2
    fi
    exit 1
}

if ! usable_display; then
    if ! recover_from_tmux; then
        if [ -n "${WAYLAND_DISPLAY:-}" ]; then
            no_display "  no socket at $(socket_for "$WAYLAND_DISPLAY") — that compositor is gone."
        else
            no_display "  neither is set, so there is nothing to connect to."
        fi
    fi
fi

"$root/.scripts/build.sh"

exec "$root/build/bin/omafiles-studio" "$@"
