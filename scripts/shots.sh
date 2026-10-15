#!/usr/bin/env bash
# Write the pictures in docs/img/ from the studio itself.
#
# Offscreen, so it needs no session and gives the same image on any machine:
# a screenshot pasted in by hand goes stale the first time a column moves, and
# nobody notices until somebody reads the page.
#
# Regenerated the same way docs/cli.md is, and checked the same way: the gate
# runs this into a scratch directory and compares. Called with an argument, it
# writes there instead and leaves docs/img alone, which is what the check uses.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${OMAFILES_TEST_ENV:-}" ]; then
    exec /usr/bin/python3 "$root/scripts/test-environment.py" "$root/scripts/shots.sh" "$@"
fi

out="${1:-$root/docs/img}"
build="$root/build-tests/studio"

mkdir -p "$out" "$build"
(
    cd "$build"
    qmake6 "$root/tests/studio.pro"
    make -j"$(nproc 2>/dev/null || echo 1)"
    OMAFILES_DOCS=1 OMAFILES_SHOTS="$out" \
        QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= \
        ./tst_studio writesTheDocumentationShots
)

for shot in "$out"/*.png; do
    printf '%s  %s\n' "$(basename "$shot")" "$(identify -format '%wx%h' "$shot" 2>/dev/null || echo '?')"
done
