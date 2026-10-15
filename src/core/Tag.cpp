#include "Tag.h"

#include "Paths.h"

#include <QRegularExpression>

namespace omafiles {

bool isValidTag(const QString &tag)
{
    static const QRegularExpression re(
        QRegularExpression::anchoredPattern(QStringLiteral("[a-z0-9][a-z0-9-]{0,63}")));
    return re.match(tag).hasMatch();
}

QString asciiFolded(const QString &raw)
{
    // ASCII only, in the trim and in the fold: Unicode case mapping turns the
    // Kelvin sign into `k`, a second spelling of a name nobody can see.
    qsizetype start = 0;
    qsizetype end = raw.size();
    while (start < end && (raw.at(start) == u' ' || raw.at(start) == u'\t'))
        ++start;
    while (end > start && (raw.at(end - 1) == u' ' || raw.at(end - 1) == u'\t'))
        --end;
    QString folded = raw.mid(start, end - start);
    for (QChar &letter : folded) {
        if (letter >= u'A' && letter <= u'Z')
            letter = QChar(letter.unicode() + ('a' - 'A'));
    }
    return folded;
}

QString normalizeTag(const QString &raw, QString *error)
{
    const QString tag = asciiFolded(raw);
    if (!isValidTag(tag)) {
        if (error) {
            *error = QStringLiteral("invalid tag: %1 (expected [a-z0-9-]+, no leading '-', max 64)")
                         .arg(spelled(raw));
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
