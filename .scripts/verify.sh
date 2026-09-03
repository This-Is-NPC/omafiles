#!/usr/bin/env bash
# omafiles local gate — pre-commit and pre-push both exec this script.
#
# Idempotent: same tree, same result. Hooks must not go through `mise run`,
# because a hook that depends on a task file is a hook that changes meaning when
# the task file does; they call these scripts directly instead. `usage-check.sh`
# does use `mise exec` — that is tool resolution, not task indirection, and it is
# the only way to get the pinned `usage`. So `mise` does have to be on PATH
# wherever `git commit` runs, and usage-check says so plainly when it is not.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

"$root/.scripts/usage-check.sh"
"$root/.scripts/qml-check.sh"
"$root/.scripts/shots-check.sh"
exec "$root/.scripts/test.sh"
