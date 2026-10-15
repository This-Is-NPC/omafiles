#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${OMAFILES_TEST_ENV:-}" ]; then
    exec /usr/bin/python3 "$root/scripts/test-environment.py" "$root/scripts/usage-gen.sh" "$@"
fi

mkdir -p docs
"$root/scripts/usage-markdown.sh" docs/cli.md
