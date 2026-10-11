#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace omafiles {

/// One file the desktop opened lately, and when.
struct RecentEntry {
    QString path;
    qint64 when = 0;  // milliseconds since the epoch
};

/// Where the desktop keeps the list: `$XDG_DATA_HOME/recently-used.xbel`, the
/// bookmark file every GTK file chooser and most editors write to.
QString recentlyUsedPath();

/// What the desktop opened lately, newest first: local files that are still
/// there and nothing else, each once under its resolved path, at its newest
/// stamp. A list that is not there, or is empty, is an empty answer and not an
/// error. False, with `error`, only for a list that is there and will not read.
/// An entry the system will not describe, or under a root that is away, is left
/// out with its reason in `skipped`. `limit` stops the list at that many, and
/// no entry past it is looked at; 0 takes them all.
bool recentlyUsed(QVector<RecentEntry> *entries, QStringList *skipped, QString *error,
                  int limit = 0);

} // namespace omafiles
