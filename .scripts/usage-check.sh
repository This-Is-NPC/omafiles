#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

kdl="omafiles.usage.kdl"
out="docs/cli.md"

if [ ! -f "$kdl" ]; then
  echo "usage-check: missing $kdl" >&2
  exit 1
fi

if ! command -v mise >/dev/null 2>&1; then
  echo "usage-check: mise is not on PATH; it pins the 'usage' that generates $out" >&2
  echo "             (git hooks inherit a thin PATH -- see .scripts/verify.sh)" >&2
  exit 1
fi

mise exec -- usage lint "$kdl" >/dev/null

if [ ! -f "$out" ]; then
  echo "usage-check: missing $out (run mise run usage:gen)" >&2
  exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

mise exec -- usage g markdown -f "$kdl" --out-file "$tmp"

if ! cmp -s "$out" "$tmp"; then
  echo "usage-check: $out is stale (run mise run usage:gen)" >&2
  exit 1
fi
