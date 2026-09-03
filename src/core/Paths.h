#pragma once

#include <QString>
#include <QStringList>

namespace omafiles {

QString defaultIndexPath();
/// What the system says about a path: it is there, it is gone, or it will not
/// say. A caller acts on Missing and steps over Unknown, so `prune` cannot drop
/// a row for a file it merely was not allowed to look at.
enum class PathState { Present, Missing, Unknown };
PathState pathState(const QString &path, QString *error);
/// The path, fully resolved, or an empty string and a reason.
///
/// `state` is which of the two reasons it was, for the caller that has to tell
/// a path that is not there from one the system would not describe.
QString canonicalPath(const QString &path, QString *error, PathState *state = nullptr);
/// The named path, with everything but its last component resolved.
///
/// POSIX `unlink()` acts on the link, not its target, and `rm`, `mv` and `gio
/// trash` follow suit. `canonicalPath` resolves the last component too, so the
/// verbs that work on the path itself use this one.
QString canonicalLinkPath(const QString &path, QString *error, PathState *state = nullptr);
bool isUnderDirectory(const QString &path, const QString &directory);
/// Every name one directory holds, with the ones no path here can spell kept
/// apart.
///
/// A filename is bytes and a path here is text: the index is UTF-8 by
/// definition and so is an xattr value, so a name that is not valid UTF-8
/// belongs to neither, and Qt's own directory listing drops it before a caller
/// ever sees it. `unnameable` collects those, mangled, which is as much of them
/// as can be shown; `names` is everything else, `.` and `..` out. Either list
/// may be null.
///
/// False with `error` filled when the directory would not open or a read of it
/// stopped part way, and `state` tells a directory that is not there from one
/// the system declines to describe -- the distinction `pathState` draws, on the
/// same rule. False means the lists are short by an unknown amount, so a caller
/// answers with the failure and not with what it collected.
bool readDirectory(const QString &directory, QStringList *names, QStringList *unnameable,
                   QString *error, PathState *state = nullptr);
/// True if the file is there and the system will talk about it.
bool statable(const QString &path, QString *error);

} // namespace omafiles
