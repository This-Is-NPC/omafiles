#pragma once

#include <QString>
#include <QStringList>

namespace omafiles {

// [a-z0-9-]+, no leading '-', max 64. Input is trimmed of spaces and tabs and
// its ASCII letters lowercased; the error names no verb, the caller does.
bool isValidTag(const QString &tag);
/// `raw` trimmed of spaces and tabs, its ASCII letters lowercased and nothing else.
QString asciiFolded(const QString &raw);
QString normalizeTag(const QString &raw, QString *error);
QStringList normalizeTags(const QStringList &raw, QString *error);
QStringList uniqueSorted(QStringList tags);

} // namespace omafiles
