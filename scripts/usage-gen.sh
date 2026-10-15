#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${OMAFILES_TEST_ENV:-}" ]; then
    exec /usr/bin/python3 "$root/scripts/test-environment.py" "$root/scripts/usage-gen.sh" "$@"
fi

mkdir -p docs
/usr/bin/python3 "$root/scripts/usage.py" g markdown -f omafiles.usage.kdl --out-file docs/cli.md
