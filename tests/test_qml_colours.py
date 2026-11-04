#!/usr/bin/env python3
"""Every colour in the studio's QML is an opaque Theme role (docs/design.md §3):
no colour literal, no alpha wash and no fade on a border colour. A transparent
Rectangle whose border width is left unset punches a hole in the window's
alpha channel, and a translucent colour shows whatever is under it."""

import re
import sys
import unittest
from pathlib import Path

QML = Path(__file__).resolve().parent.parent / "src" / "studio" / "qml"

FORBIDDEN = (
    # A colour literal: a name such as "transparent" or "#rgb" up to "#aarrggbb".
    (re.compile(r'"(?:transparent|#[0-9A-Fa-f]{3,8})"'), "colour literal"),
    # A colour built in QML, which is where an alpha would come back.
    (re.compile(r"\bQt\.(?:rgba|hsla|hsva|alpha|tint|lighter|darker)\s*\("), "computed colour"),
    (re.compile(r"\bTheme\.fill\s*\("), "Theme.fill"),
    # Reading the border group instantiates it, which is half of the hole.
    (re.compile(r"\bBehavior\s+on\s+border\."), "border animation"),
)
# Outlines: each sets its border width and draws nothing inside it.
OUTLINES = {
    ("FocusRing.qml", 'color: "transparent"'),
    ("RubberBand.qml", 'color: "transparent"'),
    ("FileDropArea.qml", 'color: "transparent"'),
}


def violations(name, text):
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0].strip()
        if (name, code) in OUTLINES:
            continue
        for pattern, what in FORBIDDEN:
            if pattern.search(code):
                found.append(f"{name}:{number}: {what}: {line.strip()}")
    return found


class QmlColoursTest(unittest.TestCase):
    def test_colours_are_opaque_theme_roles(self):
        found = [hit for path in sorted(QML.glob("*.qml"))
                 for hit in violations(path.name, path.read_text())]
        self.assertEqual(found, [], "colours outside the opaque roles:\n" + "\n".join(found))

    def test_every_form_is_found(self):
        sample = ("Rectangle {\n"
                  '    color: "transparent"\n'
                  '    border.color: "#7aa2f7"\n'
                  "    color: Theme.fill(Theme.accent, 0.18)\n"
                  "    color: Qt.rgba(0, 0, 0, 0.5)\n"
                  "    Behavior on border.color { ColorAnimation { duration: 90 } }\n"
                  "    color: Theme.chrome\n"
                  '    text: "#1"  // "#ffffff" in a comment\n'
                  "}\n")
        self.assertEqual([hit.split(":")[1] for hit in violations("sample", sample)],
                         ["2", "3", "4", "5", "6"])
        self.assertEqual(violations("FocusRing.qml", 'Rectangle {\n    color: "transparent"\n}\n'), [])


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)
