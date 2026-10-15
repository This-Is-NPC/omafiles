#!/usr/bin/env bash
# Write the CLI reference generated from omafiles.usage.kdl to <out>, with the
# version line marked for release-please, which moves the version there too.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:?usage: usage-markdown.sh <out>}"

/usr/bin/python3 "$root/scripts/usage.py" g markdown -f "$root/omafiles.usage.kdl" --out-file "$out"
sed -i -E 's|^(- \*\*Version:\*\* [0-9][^ ]*)$|\1 <!-- x-release-please-version -->|' "$out"
