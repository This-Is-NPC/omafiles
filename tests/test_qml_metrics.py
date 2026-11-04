#!/usr/bin/env python3
"""Every type size, margin, spacing, padding and height in the studio's QML
reads a metric from Theme (docs/design.md §3); only 0 and 1 px are literal.
The metrics table in docs/design.md §3 gives each metric Theme's value."""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
QML = ROOT / "src" / "studio" / "qml"
THEME = ROOT / "src" / "studio" / "Theme.h"
DESIGN = ROOT / "docs" / "design.md"

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


def theme_metrics(text):
    """Each `static constexpr int name() { return ...; }` in Theme.h, resolved
    where one metric returns another."""
    bodies = dict(re.findall(r"static constexpr int (\w+)\(\) \{ return (\w+)(?:\(\))?; \}", text))

    def value(name):
        body = bodies[name]
        return int(body) if body.isdigit() else value(body)
    return {name: value(name) for name in bodies}


def table_metrics(text):
    """The metrics the "Studio metrics" table names, with their pixels."""
    section = text.split("### Studio metrics", 1)[1].split("\n### ", 1)[0]
    found = {}
    for line in section.splitlines():
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) != 4 or not cells[1].startswith("`"):
            continue
        names = re.findall(r"`(\w+)`", cells[1])
        pixels = [int(number) for number in re.findall(r"\d+", cells[2])]
        found.update(zip(names, pixels) if len(names) == len(pixels) else {cells[1]: None})
    return found


class QmlMetricsTest(unittest.TestCase):
    def test_design_table_matches_theme(self):
        self.assertEqual(table_metrics(DESIGN.read_text()), theme_metrics(THEME.read_text()))

    def test_table_and_theme_are_read_whole(self):
        theme = ("static constexpr int spacingS() { return 8; }\n"
                 "static constexpr int inset() { return spacingS(); }\n")
        self.assertEqual(theme_metrics(theme), {"spacingS": 8, "inset": 8})
        table = ("### Studio metrics\n\n| Group | Metric | Pixels | Used for |\n|---|---|---|---|\n"
                 "| Spacing | `spacingS` `inset` | 8 8 | gaps |\n| | `fontBody` | 13 | rows |\n"
                 "| | `a` `b` | 1 | broken |\n\n### Next\n| | `after` | 2 | not read |\n")
        self.assertEqual(table_metrics(table),
                         {"spacingS": 8, "inset": 8, "fontBody": 13, "`a` `b`": None})

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
