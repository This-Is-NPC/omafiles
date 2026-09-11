#include "Recent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>

namespace omafiles {

QString recentlyUsedPath()
{
    QString dataHome = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    if (dataHome.isEmpty())
        dataHome = QDir::homePath() + QStringLiteral("/.local/share");
    return QDir(dataHome).filePath(QStringLiteral("recently-used.xbel"));
}

namespace {

// The newest of the three stamps a bookmark carries. `visited` is the last
// open, `modified` the last change to the entry, `added` the first time; a
// reader wants the last time the file mattered, which is the latest of them.
qint64 stampOf(const QXmlStreamAttributes &attributes)
{
    qint64 latest = 0;
    for (const char *name : {"visited", "modified", "added"}) {
        const QString value = attributes.value(QLatin1String(name)).toString();
        if (value.isEmpty())
            continue;
        const QDateTime when = QDateTime::fromString(value, Qt::ISODateWithMs);
        if (when.isValid())
            latest = std::max(latest, when.toSecsSinceEpoch());
    }
    return latest;
}

} // namespace

bool recentlyUsed(QVector<RecentEntry> *entries, QString *error)
{
    entries->clear();
    QFile file(recentlyUsedPath());
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(file.fileName(), file.errorString());
        return false;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement
            || xml.name() != QLatin1String("bookmark"))
            continue;
        const QUrl href(xml.attributes().value(QLatin1String("href")).toString());
        // Local files only: the list also names the documents a browser
        // fetched, and a path this program cannot stand in is not a row.
        if (!href.isLocalFile())
            continue;
        const QString path = href.toLocalFile();
        if (!QFileInfo::exists(path))
            continue;
        entries->append({path, stampOf(xml.attributes())});
    }
    if (xml.hasError()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(file.fileName(), xml.errorString());
        return false;
    }
    std::sort(entries->begin(), entries->end(), [](const RecentEntry &a, const RecentEntry &b) {
        if (a.when != b.when)
            return a.when > b.when;
        return a.path < b.path;
    });
    return true;
}

} // namespace omafiles
