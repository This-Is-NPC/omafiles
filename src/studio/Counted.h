#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {

/// A count before its noun, the last word plural unless the count is one:
/// `1 entry`, `2 entries`, `3 selected files`.
QString countOf(qint64 n, const QString &noun);

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
