#!/usr/bin/env bash
# The pictures in docs/img are what the studio draws today, not what it drew
# once. Same discipline as docs/cli.md: regenerate into a scratch directory and
# compare. Idempotent.
set -euo pipefail
# So an empty directory yields nothing rather than the literal `*.png`.
shopt -s nullglob

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/elsewhere"

# Deliberately under a TMPDIR that is not the default. The fixture path is
# rendered into the address bar of every picture, so anything that reads TMPDIR
# to choose it produces different bytes on a machine that sets one -- and the
# byte-compare below then refuses a commit for a reason that has nothing to do
# with the commit. Regenerating under an odd TMPDIR here is what makes that a
# failure on this machine rather than only on somebody else's.
TMPDIR="$tmp/elsewhere" "$root/.scripts/shots.sh" "$tmp" >/dev/null

stale=()
for fresh in "$tmp"/*.png; do
    committed="$root/docs/img/$(basename "$fresh")"
    if [ ! -f "$committed" ] || ! cmp -s "$fresh" "$committed"; then
        stale+=("$(basename "$fresh")")
    fi
done

# The other direction: a picture in docs/img that nothing generates any more is
# just as stale, and comparing only what was generated would never notice it.
for committed in "$root"/docs/img/*.png; do
    [ -f "$tmp/$(basename "$committed")" ] || stale+=("$(basename "$committed") (nothing generates it)")
done

if [ ${#stale[@]} -ne 0 ]; then
    echo "shots-check: docs/img is stale (run mise run shots): ${stale[*]}" >&2
    exit 1
fi
