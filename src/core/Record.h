#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace omafiles {

// One row of the index: a path, the tags on it, and when they last changed.
//
// A file moved behind the program's back is recovered by `reindex` plus
// `prune`, which is why no identity of the file itself is kept here: inode
// numbers are reused after a delete and device numbers move under LVM, while
// the tags also live in the file's own xattr, which a `mv` carries along.
struct Record {
    /// Where the file is, absolute and resolved. Always filled on a row that
    /// loaded: it is what every verb works in, and what the store keys on.
    QString path;
    /// How the row is written down: the id of the root holding it and the rest
    /// of the way there, or both empty for a row stored as an absolute path.
    ///
    /// Two halves rather than one string because that is the whole point of a
    /// root. Remounting the same disk somewhere else changes `path` for every
    /// row on it and changes neither of these, so the tags stay where they were
    /// -- and two machines that mount the same NAS in different places are
    /// talking about the same rows.
    QString root;
    QString relative;
    QStringList tags;

    /// When these tags last changed, in seconds since the epoch. Zero means
    /// nothing is known -- an imported row may omit it -- and is left out of
    /// the JSON rather than written as a row claiming 1970.
    ///
    /// Not the file's mtime, which answers "when did this change on disk". A
    /// tag index has to answer "what did I tag this week", a different question
    /// about the same row.
    qint64 tagged = 0;

    /// The row as a verb prints it: `path` absolute, `tags`, and `tagged`
    /// where there is one.
    QJsonObject toJson() const;
    /// The row as the index file and `export` write it: under a declared root,
    /// `root` and `path` relative to it, so the line survives a remount.
    QJsonObject toStoredJson() const;
    static Record fromJson(const QJsonObject &object, QString *error);

    /// The same row: one file, its tags and its stamp, in whichever form it is
    /// stored.
    bool operator==(const Record &other) const;
    bool operator!=(const Record &other) const { return !(*this == other); }
};

} // namespace omafiles
