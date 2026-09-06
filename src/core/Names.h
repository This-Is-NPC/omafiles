#pragma once

#include <QString>
#include <QStringList>

namespace omafiles {

struct Root;

// What is on the disk, as opposed to what you marked.
//
// The index is the second of those: it has human authorship, a mirror in each
// file's own xattr, and it is the thing worth backing up. This is the first,
// and it is a cache in the strict sense -- every line of it can be rebuilt by
// walking the disk again, nothing here is anybody's answer to anything, and
// deleting the whole directory costs one sweep and no information. That is why
// it lives under `$XDG_CACHE_HOME` and why no verb refuses to run without it.
//
// What it buys is `--untagged` without a walk. The index knows only rows that
// carry a tag, so "what have I not filed yet" means walking the tree on every
// question -- seconds, every time, for an answer that changes by the hour. A
// sorted list of names is what `locate` has answered out of for thirty years,
// and half a million paths is about thirty megabytes of it.
//
// One file per root, because that is the unit the walk is scoped to, the unit
// the storage arrives and leaves in, and the unit somebody would delete. The
// paths inside are relative to the root for the same reason the index rows are:
// remounting the same disk somewhere else must not invalidate a thing.

/// Where one root's list lives. The id is the file name, which is why a root id
/// has to be a name a filesystem will take -- see `normalizeRootId`.
QString namesCachePath(const QString &rootId);

/// Replaces one root's list. `relatives` is every path under the root, written
/// relative to it; it is sorted and de-duplicated here, so a caller hands over
/// whatever its walk produced.
///
/// A name holding a newline is left out and counted in `dropped`: the file is
/// one path to a line, and there is no escape in it that would still let a
/// reader `grep` the thing. Such a name is legal on every Unix and is vanishing
/// rare; the alternative is a format nobody can read with the tools they have.
bool writeNames(const QString &rootId, const QStringList &relatives, int *dropped,
                QString *error);

/// One root's list, as absolute paths under where the root is mounted now.
///
/// `ageSeconds` is how old the list is, from the file's own timestamp -- the
/// answer is calculated rather than stored, the way everything else here is.
/// A root nothing has swept yet is not a failure: `paths` comes back empty,
/// `ageSeconds` negative, and the caller says so rather than answering out of
/// a list that does not exist.
bool readNames(const Root &root, QStringList *paths, qint64 *ageSeconds, QString *error);

/// Whether a root has a list at all, without reading it.
bool hasNames(const QString &rootId);

/// Drops one root's list. For `roots rm`: a cache for a root nobody declares is
/// a file nothing will ever read again.
void forgetNames(const QString &rootId);

} // namespace omafiles
