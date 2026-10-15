#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace omafiles {

struct Root;
class Store;

/// What a walk could not cover: the path it is about, empty where it is about
/// the whole list, and the sentence that says so.
struct Gap {
    QString path;
    QString said;
};

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

/// Whether a list of one path to a line can hold `relative`: not where it holds
/// a newline.
bool listable(const QString &relative);

/// Replaces one root's list. `relatives` is every path under the root, written
/// relative to it; it is sorted and de-duplicated here, so a caller hands over
/// whatever its walk produced. `gaps` is what that walk could not cover, one
/// sentence each, kept in the list so an answer out of it can say it is short.
///
/// A name not `listable` is left out, counted in `dropped`, and becomes a
/// gap of its own: the file is one path to a line, and there is no escape in it
/// that would still let a reader `grep` the thing.
///
/// A gap is a line starting with `/`, which no relative path can, then its path
/// percent-encoded, a space, and the sentence.
bool writeNames(const QString &rootId, const QStringList &relatives, QVector<Gap> gaps,
                int *dropped, QString *error);

/// One root's list, as absolute paths under where the root is mounted now.
///
/// `ageSeconds` is how old the list is, from the file's own timestamp -- the
/// answer is calculated rather than stored, the way everything else here is.
/// A root nothing has swept yet is not a failure: `paths` comes back empty,
/// `ageSeconds` negative, and the caller says so rather than answering out of
/// a list that does not exist. `gaps`, where asked for, is what the sweep that
/// built the list could not cover -- see `writeNames`. Where `rows` is given, a
/// path that names none of them and is one `foldedName` from one is spelled as
/// its directory stores it now, so a file another program re-cased since the
/// sweep keeps its row; no other path is looked at.
bool readNames(const Root &root, QStringList *paths, qint64 *ageSeconds, QString *error,
               QVector<Gap> *gaps = nullptr, const Store *rows = nullptr);

/// An age, `ageSeconds` for one, in the spelling `ls --since` accepts.
QString spelledDuration(qint64 seconds);

/// Drops one root's list: for `roots rm`, a cache for a root nobody declares is
/// a file nothing will ever read again, and for `roots add`, one left behind is
/// not the new root's. False with the system's reason where it stays.
bool forgetNames(const QString &rootId, QString *error);

} // namespace omafiles
