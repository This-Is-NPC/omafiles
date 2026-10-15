#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace omafiles {

// A file's own `user.omafiles.tags`, never through a link: every path handed
// here is resolved already, and a `user.*` xattr cannot sit on a link.
enum class TagsValue {
    /// No value, or one of no bytes.
    None,
    Held,
    /// A file that cannot hold one: its filesystem has no `user.*` namespace, or
    /// it is neither a regular file nor a directory. The row is then the only
    /// record, as `cp -a` leaves such a target without.
    Unsupported,
    /// The system would not say. Never read as `None`: a write would destroy it.
    Unknown,
};

TagsValue readTagsValue(const QString &path, QByteArray *value, QString *why);
/// The same, of an open file.
TagsValue readTagsValue(int fd, QByteArray *value, QString *why);
/// The value split at every comma byte, so `,` is two empty words and no tag;
/// a word that is not UTF-8 or holds a NUL, which no tag can be, is read by
/// `surrogateEscaped`.
QStringList tagsOfValue(const QByteArray &value);

/// `readTagsValue` and `tagsOfValue` in one, the state in `state` where it is
/// not null; false where the value is `Unknown`.
bool readTagsXattr(const QString &path, QStringList *tags, QString *warning,
                   TagsValue *state = nullptr);
bool writeTagsXattr(const QString &path, const QStringList &tags, QString *warning);
/// `value` onto an open file, verbatim.
bool writeTagsValue(int fd, const QByteArray &value, QString *why);

} // namespace omafiles
