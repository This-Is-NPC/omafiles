#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ -z "${OMAFILES_TEST_ENV:-}" ]; then
    exec /usr/bin/python3 "$root/scripts/test-environment.py" "$root/scripts/usage-check.sh" "$@"
fi

kdl="omafiles.usage.kdl"
out="docs/cli.md"

if [ ! -f "$kdl" ]; then
  echo "usage-check: missing $kdl" >&2
  exit 1
fi

usage=(/usr/bin/python3 "$root/scripts/usage.py")
"${usage[@]}" lint "$kdl" >/dev/null

if [ ! -f "$out" ]; then
  echo "usage-check: missing $out (run scripts/usage-gen.sh)" >&2
  exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

"${usage[@]}" g markdown -f "$kdl" --out-file "$tmp"

if ! cmp -s "$out" "$tmp"; then
  echo "usage-check: $out is stale (run scripts/usage-gen.sh)" >&2
  exit 1
fi
