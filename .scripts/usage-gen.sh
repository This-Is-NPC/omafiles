#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

mkdir -p docs
mise exec -- usage g markdown -f omafiles.usage.kdl --out-file docs/cli.md
