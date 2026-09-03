#!/usr/bin/env bash
# Build and run the unit tests, the studio keyboard tests and the CLI e2e.
# Idempotent.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

"$root/.scripts/build.sh"

# Each .pro gets its own build directory: two qmake projects in one directory
# would write over each other's Makefile.
run_suite() {
    local project="$1" binary="$2" dir="$root/build-tests/$3"
    mkdir -p "$dir"
    (
        cd "$dir"
        qmake6 "$root/tests/$project"
        make -j"$(nproc 2>/dev/null || echo 1)"
        QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME= "./$binary"
    )
}

run_suite tests.pro tst_omafiles store
run_suite studio.pro tst_studio studio

export OMAFILES_CLI="$root/build/bin/omafiles"
python3 "$root/tests/test_cli.py"
