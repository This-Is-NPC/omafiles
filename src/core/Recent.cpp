#include "Recent.h"

#include "Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace omafiles {

QString recentlyUsedPath()
{
    return QDir(dataHome()).filePath(QStringLiteral("recently-used.xbel"));
}

namespace {

// The newest of the three stamps a bookmark carries. `visited` is the last
// open, `modified` the last change to the entry, `added` the first time; a
// reader wants the last time the file mattered, which is the latest of them.
// GLib writes microseconds; the milliseconds Qt keeps order two opens in a second.
qint64 stampOf(const QXmlStreamAttributes &attributes)
{
    qint64 latest = 0;
    for (const char *name : {"visited", "modified", "added"}) {
        const QString value = attributes.value(QLatin1String(name)).toString();
        if (value.isEmpty())
            continue;
        const QDateTime when = QDateTime::fromString(value, Qt::ISODateWithMs);
        if (when.isValid())
            latest = std::max(latest, when.toMSecsSinceEpoch());
    }
    return latest;
}

} // namespace

bool recentlyUsed(QVector<RecentEntry> *entries, QStringList *skipped, QString *error,
                  int limit)
{
    entries->clear();
    const QString path = recentlyUsedPath();
    QFile file(path);
    const PathState opened = openToRead(&file, {}, error);
    if (opened == PathState::Missing)
        return true;
    if (opened != PathState::Present)
        return false;
    // Zero bytes is a list nothing has written yet, the same as no file.
    if (file.size() == 0)
        return true;
    QVector<RecentEntry> listed;
    QXmlStreamReader xml(&file);
    bool rooted = false;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        // GLib's bookmark parser takes an `xbel` document and no other.
        if (!rooted && xml.name() != QLatin1String("xbel")) {
            xml.raiseError(QStringLiteral("Unexpected tag '%1', tag 'xbel' expected")
                               .arg(xml.name()));
            break;
        }
        rooted = true;
        if (xml.name() != QLatin1String("bookmark"))
            continue;
        const QUrl href(xml.attributes().value(QLatin1String("href")).toString());
        // Local files only: the list also names the documents a browser
        // fetched.
        const QString local = localFileOf(href);
        if (local.isEmpty())
            continue;
        listed.append({local, stampOf(xml.attributes())});
    }
    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(spelled(path), xml.errorString());
        return false;
    }
    std::stable_sort(listed.begin(), listed.end(),
                     [](const RecentEntry &a, const RecentEntry &b) { return a.when > b.when; });

    // Ordered first and looked at after, so an entry past the limit costs
    // nothing. Each file once, under the path a row names it by, at its
    // newest stamp, and each note once, for the file as far as it resolves.
    QSet<QString> seen;
    QSet<QString> noted;
    const auto note = [&](const QString &file, const QString &why) {
        if (skipped && !noted.contains(file)) {
            noted.insert(file);
            skipped->append(why);
        }
    };
    for (const RecentEntry &entry : std::as_const(listed)) {
        if (limit > 0 && entries->size() >= limit)
            break;
        QString why;
        PathState state = PathState::Missing;
        // A file no path here can spell is left out, whether the href or a
        // link on the way names it, as a URL that is not a local file is.
        bool unnameable = false;
        QString resolved = realPath(entry.path, &why, &state, &unnameable);
        if (unnameable)
            continue;
        // The file as far as it resolves, so two spellings of one are one note.
        const QString file = resolved.isEmpty() ? physicalPath(entry.path) : resolved;
        if (!resolved.isEmpty() && (state = storageState(resolved, &why)) != PathState::Present)
            resolved.clear();
        // A file where a directory above it should be is gone, as for a row.
        if (resolved.isEmpty()) {
            const bool gone = state == PathState::Missing
                || (state == PathState::Unknown && leadsNowhere(entry.path));
            if (!gone)
                note(file, why);
            continue;
        }
        if (seen.contains(resolved))
            continue;
        seen.insert(resolved);
        entries->append({resolved, entry.when});
    }
    return true;
}

} // namespace omafiles
