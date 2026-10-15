#!/usr/bin/env bash
# omafiles local gate — pre-commit and pre-push both exec this script.
#
# Hooks call scripts directly; validation executes only the installed, pinned
# usage binary in a disposable environment, without mise configuration loading.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${OMAFILES_TEST_ENV:-}" ]; then
    exec /usr/bin/python3 "$root/scripts/test-environment.py" "$root/scripts/check.sh" "$@"
fi

"$root/scripts/usage-check.sh"
/usr/bin/python3 "$root/scripts/link-check.py"
"$root/scripts/qml-check.sh"
"$root/scripts/shots-check.sh"
exec "$root/scripts/test.sh"
