#include "Paths.h"

#include "Roots.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace omafiles {

QString defaultIndexPath()
{
    QString dataHome = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    if (dataHome.isEmpty())
        dataHome = QDir::homePath() + QStringLiteral("/.local/share");
    return QDir(dataHome).filePath(QStringLiteral("omafiles/index.jsonl"));
}

namespace {

// Which of the two answers a failed syscall was.
//
// ENOENT is "no such name"; ENOTDIR is "a component of the path is not a
// directory", which cannot hold the name either -- exactly git's
// `is_missing_file_error()`. Everything else, EACCES through a dead NFS
// mount's ESTALE, is the system declining to answer, and a decline is not a no.
PathState stateForErrno(int failure)
{
    return (failure == ENOENT || failure == ENOTDIR) ? PathState::Missing : PathState::Unknown;
}

// The same answer, with the one question `errno` cannot be asked put to it
// first: a name that is not there because the disk holding it is not here is
// not a name that was deleted. `ENOENT` is what both look like from a `stat`,
// and telling them apart is what a declared root is for.
PathState stateForErrno(int failure, const QString &path)
{
    const PathState plain = stateForErrno(failure);
    if (plain == PathState::Missing && underUnreachableRoot(path))
        return PathState::Unreachable;
    return plain;
}

// Which end of the name is not there.
//
// A dangling symlink is a name that is there pointing at one that is not, so
// `path missing` would be false about the name and say nothing about the
// target. The verbs that reach this are the ones that read through a link --
// `cp` for the bytes, every `tag` verb because a `user.*` xattr cannot sit on a
// link at all -- and for them the far end is the whole of what they wanted, so
// the answer is still 2. It just says which end is empty, and names the target
// the reader has to create or repoint.
QString missing(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isSymLink())
        return QStringLiteral("path missing: %1").arg(path);
    return QStringLiteral("%1 is a symlink to %2, which is not there")
        .arg(path, info.symLinkTarget());
}

// The answer for a path that would not resolve, in both halves.
//
// "It is not there" and "the system will not say" are different answers: the
// first is `missing` above and the caller's exit 2, the second names what the
// system said and is an answer the run could not finish. `probe` is the
// component that failed, which for a symlink is its parent; `path` is what the
// caller named.
QString unresolved(const QString &probe, const QString &path, QString *error, PathState *state)
{
    QString detail;
    PathState found = pathState(probe, &detail);
    if (found == PathState::Present) {
        // It stats and still does not resolve, so it is not a name that is not
        // there either.
        found = PathState::Unknown;
        detail = QStringLiteral("%1: cannot be resolved").arg(path);
    }
    if (state)
        *state = found;
    if (error) {
        if (found == PathState::Missing)
            *error = missing(path);
        else if (found == PathState::Unreachable)
            *error = unreachableReason(probe);
        else
            *error = detail;
    }
    return {};
}

} // namespace

QString canonicalPath(const QString &path, QString *error, PathState *state)
{
    // The state is asked first and the resolution second: `realpath` answers an
    // EACCES by handing the path back unchanged, so a path nobody may look at
    // resolves to itself and every verb acts on it.
    QString canonical;
    if (statable(path, nullptr))
        canonical = QFileInfo(path).canonicalFilePath();
    if (canonical.isEmpty())
        return unresolved(path, path, error, state);
    if (state)
        *state = PathState::Present;
    return canonical;
}

QString canonicalLinkPath(const QString &path, QString *error, PathState *state)
{
    const QFileInfo info(path);
    if (state)
        *state = PathState::Present;
    // Only a symlink differs; everything else resolves through `canonicalPath`,
    // so two spellings of one directory cannot become two index rows.
    if (!info.isSymLink())
        return canonicalPath(path, error, state);
    // The prefix still resolves; only the name the link is filed under stays.
    const QString parent = canonicalPath(info.absolutePath(), nullptr, nullptr);
    const QString name = info.fileName();
    // A dangling link is still a path that is there, and `exists()` follows the
    // link, so the parent is what gets asked.
    if (parent.isEmpty() || name.isEmpty())
        return unresolved(info.absolutePath(), path, error, state);
    return QDir(parent).filePath(name);
}

bool isUnderDirectory(const QString &path, const QString &directory)
{
    if (path == directory)
        return true;
    const QString prefix = directory.endsWith(QLatin1Char('/')) ? directory
                                                                : directory + QLatin1Char('/');
    return path.startsWith(prefix);
}

PathState pathState(const QString &path, QString *error)
{
    struct stat st {};
    if (::stat(path.toLocal8Bit().constData(), &st) == 0)
        return PathState::Present;
    // Kept before anything else can overwrite it: building the message below
    // calls into Qt, and errno belongs to the stat.
    const int failure = errno;
    const PathState found = stateForErrno(failure, path);
    if (error) {
        // `No such file or directory` is what the system said and it is not
        // what happened: the name is absent because the storage is, and a
        // reader told the first goes looking for a deletion that never was.
        *error = found == PathState::Unreachable
            ? unreachableReason(path)
            : QStringLiteral("%1: %2").arg(path, QString::fromLocal8Bit(strerror(failure)));
    }
    return found;
}

bool readDirectory(const QString &directory, QStringList *names, QStringList *unnameable,
                   QString *error, PathState *state)
{
    DIR *handle = ::opendir(QFile::encodeName(directory).constData());
    if (!handle) {
        const int failure = errno;
        if (error)
            *error = QStringLiteral("%1: %2").arg(directory,
                                                  QString::fromLocal8Bit(strerror(failure)));
        if (state)
            *state = stateForErrno(failure, directory);
        return false;
    }
    if (state)
        *state = PathState::Present;
    while (true) {
        // Zeroed before every read, not once before the loop: `readdir` returns
        // null for the end of the directory and for a failure alike, errno is
        // the only thing that tells them apart, and anything the body calls may
        // have written to it in between.
        errno = 0;
        const dirent *entry = ::readdir(handle);
        if (!entry)
            break;
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        const QString spelled = QFile::decodeName(name);
        // The round trip is the test: a name that does not come back as the
        // bytes it went in as is one no path here can be spelled from.
        if (QFile::encodeName(spelled) != name) {
            if (unnameable)
                unnameable->append(spelled);
            continue;
        }
        if (names)
            names->append(spelled);
    }
    // Kept before `closedir`, which has an errno of its own to set.
    const int failure = errno;
    ::closedir(handle);
    if (failure != 0) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(directory,
                                                  QString::fromLocal8Bit(strerror(failure)));
        if (state)
            *state = stateForErrno(failure, directory);
        return false;
    }
    return true;
}

bool statable(const QString &path, QString *error)
{
    return pathState(path, error) == PathState::Present;
}

} // namespace omafiles
