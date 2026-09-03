#pragma once

#include <QString>
#include <QStringList>

namespace omafiles {

bool writeTagsXattr(const QString &path, const QStringList &tags, QString *warning);
bool readTagsXattr(const QString &path, QStringList *tags, QString *warning);

} // namespace omafiles
