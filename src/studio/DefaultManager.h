#pragma once

#include <QString>

namespace omafiles {
struct DefaultManagerResult {
    bool ok = false;
    QString text;
};
DefaultManagerResult makeDefaultFileManager(const QString &executable);
} // namespace omafiles
