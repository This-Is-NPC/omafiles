#pragma once

#include <QString>
#include <QStringList>

namespace omafiles {

// [a-z0-9-]+, no leading '-', max 64. Input is lowercased.
bool isValidTag(const QString &tag);
QString normalizeTag(const QString &raw, QString *error);
QStringList normalizeTags(const QStringList &raw, QString *error);
QStringList uniqueSorted(QStringList tags);

} // namespace omafiles
