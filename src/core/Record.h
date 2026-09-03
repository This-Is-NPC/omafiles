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
    QString path;
    QStringList tags;

    /// When these tags last changed, in seconds since the epoch. Zero means
    /// nothing is known -- an imported row may omit it -- and is left out of
    /// the JSON rather than written as a row claiming 1970.
    ///
    /// Not the file's mtime, which answers "when did this change on disk". A
    /// tag index has to answer "what did I tag this week", a different question
    /// about the same row.
    qint64 tagged = 0;

    QJsonObject toJson() const;
    static Record fromJson(const QJsonObject &object, QString *error);
};

} // namespace omafiles
