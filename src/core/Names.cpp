#include "Names.h"

#include "Roots.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>

namespace omafiles {

namespace {

QString cacheHome()
{
    QString home = QString::fromLocal8Bit(qgetenv("XDG_CACHE_HOME"));
    if (home.isEmpty())
        home = QDir::homePath() + QStringLiteral("/.cache");
    return QDir(home).filePath(QStringLiteral("omafiles/names"));
}

} // namespace

QString namesCachePath(const QString &rootId)
{
    return QDir(cacheHome()).filePath(rootId);
}

bool writeNames(const QString &rootId, const QStringList &relatives, int *dropped,
                QString *error)
{
    const QString path = namesCachePath(rootId);
    QDir().mkpath(QFileInfo(path).absolutePath());

    QStringList lines;
    lines.reserve(relatives.size());
    int lost = 0;
    for (const QString &relative : relatives) {
        if (relative.isEmpty())
            continue;                        // the root itself is not under itself
        if (relative.contains(QLatin1Char('\n'))) {
            ++lost;
            continue;
        }
        lines.append(relative);
    }
    if (dropped)
        *dropped = lost;
    // Sorted here rather than trusted from the caller: everything that reads
    // this is allowed to assume the order, and a walk's order is the
    // filesystem's business.
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    // By rename, the way the index and the roots are written. A cache is
    // disposable and a half-written one is not: a reader that caught this file
    // mid-write would answer out of an arbitrary prefix of the tree and call it
    // the whole answer.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("names: cannot write %1").arg(path);
        return false;
    }
    QByteArray body;
    for (const QString &line : std::as_const(lines)) {
        body += line.toUtf8();
        body += '\n';
    }
    if (file.write(body) != body.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("names: cannot write %1").arg(path);
        return false;
    }
    return true;
}

bool readNames(const Root &root, QStringList *paths, qint64 *ageSeconds, QString *error)
{
    if (paths)
        paths->clear();
    if (ageSeconds)
        *ageSeconds = -1;

    const QString path = namesCachePath(root.id);
    QFile file(path);
    // Not an error, and it must not be: it is a root nothing has swept yet, and
    // the caller's job is to say that rather than to answer out of nothing.
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("names: cannot read %1").arg(path);
        return false;
    }
    if (ageSeconds) {
        const QDateTime built = QFileInfo(path).lastModified();
        *ageSeconds = built.isValid() ? built.secsTo(QDateTime::currentDateTime()) : -1;
    }
    if (!paths)
        return true;

    const QString base = root.path.endsWith(QLatin1Char('/'))
        ? root.path
        : root.path + QLatin1Char('/');
    while (!file.atEnd()) {
        QByteArray line = file.readLine();
        while (line.endsWith('\n') || line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;
        paths->append(base + QString::fromUtf8(line));
    }
    return true;
}

bool hasNames(const QString &rootId)
{
    return QFileInfo::exists(namesCachePath(rootId));
}

void forgetNames(const QString &rootId)
{
    QFile::remove(namesCachePath(rootId));
}

} // namespace omafiles
