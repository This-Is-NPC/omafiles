#include "Counted.h"

namespace omafiles {

QString countOf(qint64 n, const QString &noun)
{
    if (n == 1)
        return QStringLiteral("1 %1").arg(noun);
    // A consonant before the final y: entry, entries; key, keys.
    const bool ies = noun.size() > 1 && noun.endsWith(QLatin1Char('y'))
                     && !QStringLiteral("aeiou").contains(noun.at(noun.size() - 2));
    return QStringLiteral("%1 %2").arg(n).arg(ies ? noun.chopped(1) + QStringLiteral("ies")
                                                  : noun + QLatin1Char('s'));
}

} // namespace omafiles
