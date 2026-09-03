#include "Xattr.h"

#include <cerrno>
#include <cstring>
#include <sys/xattr.h>

#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

namespace omafiles {
namespace {

const char *kName = "user.omafiles.tags";

QString formatWarning(const QString &path, const char *op)
{
    return QStringLiteral("xattr %1 failed on %2: %3")
        .arg(QString::fromLatin1(op), path, QString::fromLocal8Bit(strerror(errno)));
}

} // namespace

bool writeTagsXattr(const QString &path, const QStringList &tags, QString *warning)
{
    const QByteArray file = path.toLocal8Bit();
    if (tags.isEmpty()) {
        if (removexattr(file.constData(), kName) != 0 && errno != ENODATA && errno != ENOATTR) {
            if (warning)
                *warning = formatWarning(path, "remove");
            return false;
        }
        return true;
    }
    const QByteArray value = tags.join(QLatin1Char(',')).toUtf8();
    if (setxattr(file.constData(), kName, value.constData(), static_cast<size_t>(value.size()), 0) != 0) {
        if (warning)
            *warning = formatWarning(path, "set");
        return false;
    }
    return true;
}

bool readTagsXattr(const QString &path, QStringList *tags, QString *warning)
{
    const QByteArray file = path.toLocal8Bit();
    const ssize_t size = getxattr(file.constData(), kName, nullptr, 0);
    if (size < 0) {
        if (errno == ENODATA || errno == ENOATTR)
            return true;
        if (warning)
            *warning = formatWarning(path, "get");
        return false;
    }
    QByteArray value(static_cast<int>(size), Qt::Uninitialized);
    if (getxattr(file.constData(), kName, value.data(), static_cast<size_t>(size)) < 0) {
        if (warning)
            *warning = formatWarning(path, "get");
        return false;
    }
    const QString text = QString::fromUtf8(value);
    if (!text.isEmpty())
        *tags = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    return true;
}

} // namespace omafiles
