#!/usr/bin/env bash
# Build and run the unit tests, the studio keyboard tests and the CLI e2e.
# Idempotent.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${OMAFILES_TEST_ENV:-}" ]; then
    exec /usr/bin/python3 "$root/scripts/test-environment.py" "$root/scripts/test.sh" "$@"
fi

/usr/bin/python3 "$root/tests/test_test_environment.py"
/usr/bin/python3 "$root/tests/test_usage_tool.py"
/usr/bin/python3 "$root/tests/test_write_scope.py"
/usr/bin/python3 "$root/tests/test_benchmark.py"
/usr/bin/python3 "$root/tests/test_link_check.py"
/usr/bin/python3 "$root/tests/test_benchmark_observer.py"
python3 "$root/tests/test_incremental_build.py"
/usr/bin/python3 "$root/tests/test_portal_install.py"

"$root/scripts/build.sh"

# Each .pro gets its own build directory: two qmake projects in one directory
# would write over each other's Makefile.
run_suite() {
    local project="$1" binary="$2" dir="$root/build-tests/$3"
    mkdir -p "$dir"
    (
        cd "$dir"
        qmake6 "$root/tests/$project"
        make -j"$(nproc 2>/dev/null || echo 1)"
        /usr/bin/python3 "$root/scripts/test-environment.py" "./$binary"
    )
}

/usr/bin/python3 "$root/tests/device_bus.py" "$root/build/bin/omafiles-devices"

run_suite benchmark.pro tst_benchmark_probe benchmark
run_suite tests.pro tst_omafiles store
run_suite studio.pro tst_studio studio
run_suite portal.pro tst_portal portal

export OMAFILES_CLI="$root/build/bin/omafiles"
python3 "$root/tests/test_cli.py"
