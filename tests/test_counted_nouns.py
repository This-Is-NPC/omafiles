#!/usr/bin/env python3
"""Every count the studio puts before a noun is spelled by countOf
(src/studio/Counted.h), so one entry never reads `1 entries`."""

import re
import unittest
from pathlib import Path

STUDIO = Path(__file__).resolve().parent.parent / "src" / "studio"
HELPER = {"Counted.h", "Counted.cpp"}
NOUNS = (r"(?:files?|entry|entries|items?|tags?|rows?|paths?|URIs?|bytes?"
         r"|problems?|names?|folders?|marks?|lines?)\b")
# An optional word before the noun, as in `selected entries`; not a verb, as
# in `%1 contains names`.
WORD = r"(?:[a-z]+(?<!s) )?"
# A placeholder before a noun, `%1 files` or `%1 tagged rows`; a number
# concatenated before one, `+ " selected entries"`; a written-out `"1 item"`;
# a hedged plural, `row(s)`.
COUNTED_NOUN = re.compile(
    r"%\d+ " + WORD + NOUNS
    + r'|\+\s*"\s' + WORD + NOUNS
    + r'|"1 ' + WORD + NOUNS
    + r"|\w\(s\)")


def violations(name, text):
    return [f"{name}:{number}: {line.strip()}"
            for number, line in enumerate(text.splitlines(), 1)
            if COUNTED_NOUN.search(line.split("//", 1)[0])]


class CountedNounsTest(unittest.TestCase):
    def test_counts_go_through_count_of(self):
        found = [hit for path in sorted(STUDIO.rglob("*"))
                 if path.suffix in {".cpp", ".h", ".qml", ".js"} and path.name not in HELPER
                 for hit in violations(path.name, path.read_text())]
        self.assertEqual(found, [], "counts outside countOf:\n" + "\n".join(found))

    def test_every_counted_form_is_found(self):
        sample = ('QStringLiteral("published %1 entries").arg(n)\n'
                  'QStringLiteral("%1 tagged rows").arg(n)\n'
                  '"Compress " + paths.length + " selected entries"\n'
                  'n === 1 ? "1 item" : n + " items"\n'
                  'QStringLiteral("Restored %1 item(s).")\n'
                  '"Compress " + Counted.of(paths.length, "selected entry")\n'
                  '// %1 files in a comment\n')
        self.assertEqual([hit.split(":")[1] for hit in violations("x", sample)],
                         ["1", "2", "3", "4", "5"])


if __name__ == "__main__":
    unittest.main()
