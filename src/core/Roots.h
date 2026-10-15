#pragma once

#include "Paths.h"

#include <QString>
#include <QVector>

#include <optional>

namespace omafiles {

/// One declared root: a tree the index is allowed to keep rows relative to.
///
/// Declaring one buys two things. A row under it is stored as the root's id and
/// a relative path, so remounting the same disk somewhere else leaves every tag
/// where it was. And the run can tell "this file was deleted" from "this disk is
/// not plugged in", which is the difference between `prune` doing its job and
/// `prune` throwing away the tags of an entire archive.
struct Root {
    QString id;
    /// Where the tree is when it is here. Absolute.
    QString path;
    /// Whether the tree arrives by being mounted -- a disk, a NAS, an sshfs --
    /// rather than being part of the filesystem that is always there.
    bool mount = false;
    /// A path inside the root that exists only when the storage really is
    /// there. Optional, and the surest of the three answers below: an unmounted
    /// mount point is usually still an empty directory, so its being there
    /// proves nothing on its own.
    QString check;

    bool operator==(const Root &other) const
    {
        return id == other.id && path == other.path && mount == other.mount && check == other.check;
    }
};

/// The id a root may be declared under, folded, or empty with a reason.
///
/// `[a-z0-9][a-z0-9._-]{0,63}` -- the shape of a name rather than of a path.
/// Two things read an id and both of them need it to be one: every index row
/// names its root, so an id holding a `/` or a newline is a row nothing can
/// place; and the name cache is one file per root with the id for a file name,
/// so `..` would be a cache written somewhere else entirely. Folded to lower
/// case for the reason a tag is: `Archive` and `archive` naming two different
/// roots is a distinction nobody means to draw.
QString normalizeRootId(const QString &raw, QString *error);

/// `raw` as a root's check: a clean path relative to the root, with no `..`
/// out of it, or empty with a reason. An absolute check would stop naming the
/// storage the day it is mounted somewhere else.
QString normalizeRootCheck(const QString &raw, QString *error);

/// Where the declarations live: one JSON object per line, the shape the index
/// uses, so the same eyes and the same tools read both.
QString defaultRootsPath();

/// A process-wide value snapshot. Readers retain their copy while declarations
/// are replaced by another thread.
QVector<Root> roots();
QVector<Root> setRoots(QVector<Root> table);

/// Reads `path` into the table. A file that is not there is not a failure: it
/// is a machine that has declared nothing. A root's path spelled in another
/// case than its directory stores the name is read as the stored name.
bool loadRoots(const QString &path, QString *error);
bool loadRoots(QString *error);
/// Whether the last load placed a root where its directory stores the name in
/// another case than the file spells it, so writing the table back respells it.
bool rootsRespelled();
/// Writes the table back, by rename, the way the index is written.
bool saveRoots(const QString &path, const QVector<Root> &table, QString *error);

std::optional<Root> rootById(const QString &id);
/// The root holding `absolutePath`, or empty. The longest match wins, so a root
/// declared inside another one takes the rows under it.
std::optional<Root> rootFor(const QString &absolutePath);
/// The root declared at `absolutePath` itself, or empty.
std::optional<Root> rootAt(const QString &absolutePath);
/// Whether the root's storage is here: Present, Unreachable where it is away,
/// or Unknown with the system's reason where the question could not be asked.
PathState rootState(const Root &root, QString *error);
/// A sentence naming the root `path` is waiting on, for whoever has to report it.
QString unreachableReason(const QString &path, const Root &root);

/// The two halves a row is stored as: the id of the root holding `absolutePath`
/// and the rest of the way there. Both empty when no root holds it, which is
/// the row that stays absolute.
void splitByRoot(const QString &absolutePath, QString *rootId, QString *relative);
/// The path a stored row names, or empty where its root is not declared.
QString joinRoot(const QString &rootId, const QString &relative);

} // namespace omafiles
