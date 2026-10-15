#pragma once

#include <QList>
#include <QString>

namespace omafiles {
QList<int> workerCpus(const QString &procRoot = QStringLiteral("/proc"));
}
