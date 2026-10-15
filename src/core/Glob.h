#pragma once

#include <QString>

namespace omafiles {

/// Whether a name fits a shell pattern, folded: `fnmatch(3)` with
/// `FNM_CASEFOLD`, which is `find -iname`. A backslash escapes the character
/// after it and a `[` with no closing `]` is itself. The CLI's `--name` and the
/// window's `*` and `f` all ask this, so the two cannot disagree.
bool nameFits(const QString &pattern, const QString &name);

} // namespace omafiles
