#pragma once

#include <QString>

namespace omafiles {

/// A count before its noun, the last word plural unless the count is one:
/// `1 entry`, `2 entries`, `3 selected files`.
QString countOf(qint64 n, const QString &noun);

} // namespace omafiles
