#pragma once

#include <QString>
#include <QVector>

namespace omafiles {

/// One file the desktop opened lately, and when.
struct RecentEntry {
    QString path;
    qint64 when = 0;
};

/// Where the desktop keeps the list: `$XDG_DATA_HOME/recently-used.xbel`, the
/// bookmark file every GTK file chooser and most editors write to.
QString recentlyUsedPath();

/// What the desktop opened lately, newest first, local files that are still
/// there and nothing else. A list that is not there is an empty answer and not
/// an error: a machine nothing has opened anything on has nothing to say.
/// False, with `error`, only for a list that is there and will not read.
bool recentlyUsed(QVector<RecentEntry> *entries, QString *error);

} // namespace omafiles
