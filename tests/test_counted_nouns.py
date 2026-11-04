#!/usr/bin/env python3
"""Every count the command line, the core and the studio put before a noun is
spelled by countOf (src/core/Count.h), so one entry never reads `1 entries`."""

import re
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
TREES = ("cli", "core", "studio")
NOUNS = (r"(?:files?|entry|entries|items?|tags?|rows?|paths?|URIs?|bytes?"
         r"|problems?|names?|folders?|marks?|lines?|trees?)\b")
# An optional word before the noun, as in `selected entries`; not a verb, as
# in `%1 contains names`.
WORD = r"(?:[a-z]+(?<!s) )?"
# A placeholder before a noun, `%1 files` or `%1 tagged rows`; a number
# concatenated or streamed before one, `+ " selected entries"`,
# `<< " rows"`; a written-out `"1 item"`.
COUNTED_NOUN = re.compile(
    r"%\d+ " + WORD + NOUNS
    + r'|(?:\+|<<)\s*"\s' + WORD + NOUNS
    + r'|"1 ' + WORD + NOUNS)
# A hedged plural inside a string, `row(s)`; outside one, `f(s)` is a call.
HEDGED = re.compile(r"[A-Za-z]\(s\)")
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')


def violations(name, text):
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        code = line.split("//", 1)[0]
        if COUNTED_NOUN.search(code) or any(HEDGED.search(s) for s in STRING.findall(code)):
            found.append(f"{name}:{number}: {line.strip()}")
    return found


class CountedNounsTest(unittest.TestCase):
    def test_counts_go_through_count_of(self):
        found = [hit for tree in TREES for path in sorted((SRC / tree).rglob("*"))
                 if path.suffix in {".cpp", ".h", ".qml", ".js"}
                 for hit in violations(f"{tree}/{path.name}", path.read_text())]
        self.assertEqual(found, [], "counts outside countOf:\n" + "\n".join(found))

    def test_every_counted_form_is_found(self):
        sample = ('QStringLiteral("published %1 entries").arg(n)\n'
                  'QStringLiteral("%1 tagged rows").arg(n)\n'
                  '"Compress " + paths.length + " selected entries"\n'
                  'n === 1 ? "1 item" : n + " items"\n'
                  'QStringLiteral("Ejected %1 volume(s).")\n'
                  'out() << n << " row(s) " << verbed\n'
                  'out() << n << " rows written"\n'
                  '"Compress " + Counted.of(paths.length, "selected entry")\n'
                  'return settledStatus(s);\n'
                  '// %1 files in a comment\n')
        self.assertEqual([hit.split(":")[1] for hit in violations("x", sample)],
                         ["1", "2", "3", "4", "5", "6", "7"])


if __name__ == "__main__":
    unittest.main()
