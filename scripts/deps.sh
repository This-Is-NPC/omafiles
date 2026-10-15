#!/usr/bin/env bash
# Check system build deps. Idempotent: same machine, same result.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

missing=""
for tool in qmake6 g++ make pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
for header in QtCore/QCoreApplication QtTest/QtTest QtQuick/QQuickWindow QtPdf/QPdfDocument QtMultimedia/QMediaPlayer; do
  ls /usr/include/qt6/$header >/dev/null 2>&1 || missing="$missing $header"
done
pkg-config --exists KF6WindowSystem || missing="$missing KF6WindowSystem"
pkg-config --exists gio-2.0 || missing="$missing gio-2.0"
test -f /usr/include/archive.h || missing="$missing archive.h"

# Needed to run, not to build, so it is reported and does not stop anything:
# without it every verb works except `trash`, which hands the file to gio rather
# than reimplementing the desktop's wastebasket.
command -v gio >/dev/null 2>&1 \
  || echo "note: gio is not installed — 'omafiles trash' will refuse (pacman -S glib2)" >&2

if [ -z "$missing" ]; then
  echo "all present — qt $(qmake6 -query QT_VERSION), $(g++ --version | head -1)"
  exit 0
fi

echo "missing:$missing" >&2
echo >&2
echo "on Arch / Omarchy:" >&2
echo "  sudo pacman -S --needed qt6-base qt6-declarative qt6-webengine qt6-multimedia libarchive glib2 kwindowsystem gcc make" >&2
exit 1
