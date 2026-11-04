#pragma once

#include "Count.h"

#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {

/// countOf for QML, so a caption there spells counts the way C++ does.
class Counted : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    using QObject::QObject;

    Q_INVOKABLE QString of(qint64 n, const QString &noun) const { return countOf(n, noun); }
};

} // namespace omafiles
