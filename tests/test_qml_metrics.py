#!/usr/bin/env python3
"""Every type size, margin, spacing, padding and height in the studio's QML
reads a metric from Theme (docs/design.md §3); only 0 and 1 px are literal."""

import re
import sys
import unittest
from pathlib import Path

QML = Path(__file__).resolve().parent.parent / "src" / "studio" / "qml"

# A property that carries a metric, after any `anchors.`, `font.` or
# `property <type>` prefix. A sheet's preferredHeight is its extent; a
# layout's preferredHeight is a metric.
METRIC = re.compile(
    r"^(?:readonly\s+)?(?:property\s+\w+\s+)?(?:(?!Layout\.)[\w.]*\.)?"
    r"(pixelSize|margins|\w*Margin|spacing|\w*Spacing|padding|\w*Padding"
    r"|height|implicitHeight|Layout\.preferredHeight|rowHeight)\s*:(.*)$")
# An integer that is not part of a decimal or a name.
NUMBER = re.compile(r"(?<![\w.])(\d+)(?![\w.])")
# A count that scales a metric, as in `2 * Theme.inset` or `width / 2`.
FACTOR = re.compile(r"\d+\s*\*|[*/]\s*\d+")
# Extents, not metrics: the default size of a window and the cap on a preview
# image.
EXTENTS = {
    ("Main.qml", "height: 720"),
    ("Main.qml", "height: Math.min(implicitHeight, 480, Math.max(0, win.height - 2 * Theme.spacingS))"),
    ("FileChooser.qml", "height: 560"),
    ("PreviewSheet.qml", "height: hasImage ? Math.max(120, Math.min(420, panel.height - 150)) : 0"),
}


def violations(name, text):
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line).split("//", 1)[0]
        for statement in re.split(r"[{};]", code):
            statement = statement.strip()
            match = METRIC.match(statement)
            if (match and (name, statement) not in EXTENTS
                    and any(int(literal) > 1
                            for literal in NUMBER.findall(FACTOR.sub("", match.group(2))))):
                found.append(f"{name}:{number}: {line.strip()}")
    return found


class QmlMetricsTest(unittest.TestCase):
    def test_metrics_come_from_theme(self):
        found = [hit for path in sorted(QML.glob("*.qml"))
                 for hit in violations(path.name, path.read_text())]
        self.assertEqual(found, [], "literal metrics:\n" + "\n".join(found))

    def test_every_literal_form_is_found(self):
        sample = ("Item {\n"
                  "    height: 720\n"
                  "    Text {\n"
                  "        font.pixelSize: 13\n"
                  "        height: 1\n"
                  "        anchors.leftMargin: Theme.inset + 2\n"
                  "        text: \"height: 9\"\n"
                  "    }\n"
                  "    Item { width: 1; height: 6 }\n"
                  "    readonly property int rowHeight: 30\n"
                  "    Item { height: 2 * Theme.inset; anchors.margins: parent.width / 3 }\n"
                  "    Grid { columnSpacing: 10; Layout.preferredHeight: 28 }\n"
                  "    height: 0.5 * parent.height\n"
                  "}\n")
        self.assertEqual([hit.split(":")[1] for hit in violations("sample", sample)],
                         ["2", "4", "6", "9", "10", "12", "12"])
        self.assertEqual(violations("Main.qml", "Window {\n    height: 720\n}\n"), [])


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
