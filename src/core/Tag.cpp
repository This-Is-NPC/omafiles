#include "Tag.h"

#include <QRegularExpression>

namespace omafiles {

bool isValidTag(const QString &tag)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"));
    return re.match(tag).hasMatch();
}

QString normalizeTag(const QString &raw, QString *error)
{
    const QString tag = raw.trimmed().toLower();
    if (!isValidTag(tag)) {
        if (error) {
            *error = QStringLiteral("invalid tag: %1 (expected [a-z0-9-]+, no leading '-', max 64)")
                         .arg(raw);
        }
        return {};
    }
    return tag;
}

QStringList normalizeTags(const QStringList &raw, QString *error)
{
    QStringList out;
    for (const QString &item : raw) {
        const QString tag = normalizeTag(item, error);
        if (tag.isEmpty())
            return {};
        out.append(tag);
    }
    return uniqueSorted(out);
}

QStringList uniqueSorted(QStringList tags)
{
    tags.removeDuplicates();
    tags.sort();
    return tags;
}

} // namespace omafiles
