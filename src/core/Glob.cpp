#include "Glob.h"

#include <QFile>

#include <fnmatch.h>

namespace omafiles {

bool nameFits(const QString &pattern, const QString &name)
{
    return ::fnmatch(QFile::encodeName(pattern).constData(),
                     QFile::encodeName(name).constData(), FNM_CASEFOLD)
        == 0;
}

} // namespace omafiles
