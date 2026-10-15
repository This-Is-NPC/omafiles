#!/usr/bin/env bash
# Lint the QML, with every warning fatal. Idempotent.
#
# `theme`, `workspace` and `indexModel` are declared singletons and the item
# types are declared rather than registered in code, so the linter sees every
# use of them and a single warning can fail the gate.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

lint="/usr/lib/qt6/bin/qmllint"
if [ ! -x "$lint" ]; then
    lint="$(command -v qmllint || true)"
fi
if [ -z "$lint" ]; then
    echo "qml-check: no qmllint found (qt6-declarative ships it)" >&2
    exit 1
fi

# It needs the type description the build generates, under a qmldir, so that
# `import omafiles` resolves to the same five types the engine registers.
"$root/scripts/build.sh" >/dev/null

types="$root/build/src/studio/omafiles-studio.qmltypes"
if [ ! -f "$types" ]; then
    echo "qml-check: $types is missing; the build should have written it" >&2
    exit 1
fi

imports="$(mktemp -d)"
trap 'rm -rf "$imports"' EXIT
mkdir -p "$imports/omafiles"
cp "$types" "$imports/omafiles/"
cat > "$imports/omafiles/qmldir" <<EOF
module omafiles
typeinfo $(basename "$types")
depends QtQuick
depends QtCore
EOF

exec "$lint" --max-warnings 0 -I "$imports" -I "$root/src/studio/qml" \
     "$root"/src/studio/qml/*.qml
