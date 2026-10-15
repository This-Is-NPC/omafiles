#pragma once
#include <QStringList>
#include <functional>

namespace omafiles {
using ArchiveProgress = std::function<void(qint64 entries, qint64 bytes)>;
QStringList runArchive(const QStringList &paths, const QString &destination, const QString &format,
                       bool extracting, const QString &stage, const ArchiveProgress &progress);
}
