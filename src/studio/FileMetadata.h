#pragma once

#include <QFileInfo>

namespace omafiles {
// What may be done with a file, spelled the way `ls -l` spells it.
inline QString modeOf(const QFileInfo &info)
{
    const QFile::Permissions may = info.permissions();
    const auto letter = [&](QFile::Permission bit, char yes) {
        return QLatin1Char(may.testFlag(bit) ? yes : '-');
    };
    return QString(info.isSymLink() ? QLatin1Char('l')
                   : info.isDir()   ? QLatin1Char('d') : QLatin1Char('-'))
        + letter(QFile::ReadOwner, 'r') + letter(QFile::WriteOwner, 'w')
        + letter(QFile::ExeOwner, 'x') + letter(QFile::ReadGroup, 'r')
        + letter(QFile::WriteGroup, 'w') + letter(QFile::ExeGroup, 'x')
        + letter(QFile::ReadOther, 'r') + letter(QFile::WriteOther, 'w')
        + letter(QFile::ExeOther, 'x');
}

} // namespace omafiles
