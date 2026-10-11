#include "Xattr.h"

#include "Paths.h"

#include <QFile>

#include <cerrno>
#include <sys/stat.h>
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
        .arg(QString::fromLatin1(op), spelled(path), systemReason(errno));
}

bool noValue(int failure)
{
    return failure == ENODATA || failure == ENOATTR;
}

// xattr(7): a `user.*` value sits on a regular file or a directory only; any
// other file refuses one with EPERM, which is no namespace, not a refusal.
bool holdsNone(const struct stat &st)
{
    return !S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode);
}

TagsValue stateOf(int failure)
{
    return failure == ENOTSUP ? TagsValue::Unsupported
         : noValue(failure)   ? TagsValue::None
                              : TagsValue::Unknown;
}

// Sized, then read; a value that grew in between is asked for again. A value
// of no bytes is no value: writing no tags removes the attribute.
template<typename Get>
TagsValue readWith(Get get, QByteArray *value)
{
    for (;;) {
        const ssize_t size = get(nullptr, 0);
        if (size < 0)
            return stateOf(errno);
        value->resize(size);
        const ssize_t got = get(value->data(), static_cast<size_t>(size));
        if (got >= 0) {
            value->resize(got);
            return got == 0 ? TagsValue::None : TagsValue::Held;
        }
        if (errno != ERANGE)
            return stateOf(errno);
    }
}

} // namespace

TagsValue readTagsValue(const QString &path, QByteArray *value, QString *why)
{
    const QByteArray file = QFile::encodeName(path);
    if (struct stat st {}; ::lstat(file.constData(), &st) == 0 && holdsNone(st))
        return TagsValue::Unsupported;
    const TagsValue state = readWith([&file](char *into, size_t size) {
        return ::lgetxattr(file.constData(), kName, into, size);
    }, value);
    if (state == TagsValue::Unknown && why)
        *why = formatWarning(path, "get");
    return state;
}

TagsValue readTagsValue(int fd, QByteArray *value, QString *why)
{
    if (struct stat st {}; ::fstat(fd, &st) == 0 && holdsNone(st))
        return TagsValue::Unsupported;
    const TagsValue state = readWith([fd](char *into, size_t size) {
        return ::fgetxattr(fd, kName, into, size);
    }, value);
    if (state == TagsValue::Unknown && why)
        *why = systemReason(errno);
    return state;
}

QStringList tagsOfValue(const QByteArray &value)
{
    QStringList words;
    for (const QByteArray &bytes : value.split(',')) {
        QString word;
        words.append(nameFromBytes(bytes, &word) ? word : surrogateEscaped(bytes));
    }
    return words;
}

bool readTagsXattr(const QString &path, QStringList *tags, QString *warning,
                   TagsValue *state)
{
    QByteArray value;
    const TagsValue held = readTagsValue(path, &value, warning);
    if (held == TagsValue::Held)
        *tags = tagsOfValue(value);
    if (state)
        *state = held;
    return held != TagsValue::Unknown;
}

bool writeTagsXattr(const QString &path, const QStringList &tags, QString *warning)
{
    const QByteArray file = QFile::encodeName(path);
    if (tags.isEmpty()) {
        if (::lremovexattr(file.constData(), kName) == 0)
            return true;
        // A value it does not hold is nothing to refuse, and the kernel asks
        // permission before it looks, so that is asked of the read.
        const int refused = errno;
        if (::lgetxattr(file.constData(), kName, nullptr, 0) < 0
            && stateOf(errno) != TagsValue::Unknown)
            return true;
        errno = refused;
        if (warning)
            *warning = formatWarning(path, "remove");
        return false;
    }
    const QByteArray value = tags.join(QLatin1Char(',')).toUtf8();
    const auto size = static_cast<size_t>(value.size());
    if (::lsetxattr(file.constData(), kName, value.constData(), size, 0) != 0) {
        if (warning)
            *warning = formatWarning(path, "set");
        return false;
    }
    return true;
}

bool writeTagsValue(int fd, const QByteArray &value, QString *why)
{
    if (::fsetxattr(fd, kName, value.constData(), static_cast<size_t>(value.size()), 0) == 0)
        return true;
    *why = systemReason(errno);
    return false;
}

} // namespace omafiles
