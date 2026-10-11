#include "Names.h"

#include "Paths.h"
#include "Roots.h"
#include "Store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace omafiles {

QString namesCachePath(const QString &rootId)
{
    return QDir(cacheHome()).filePath(QStringLiteral("omafiles/names/") + rootId);
}

bool listable(const QString &relative)
{
    return !relative.contains(QLatin1Char('\n'));
}

bool writeNames(const QString &rootId, const QStringList &relatives, QVector<Gap> gaps,
                int *dropped, QString *error)
{
    const QString path = namesCachePath(rootId);
    QStringList lines;
    lines.reserve(relatives.size());
    int lost = 0;
    for (const QString &relative : relatives) {
        if (relative.isEmpty())
            continue;                        // the root itself is not under itself
        if (!listable(relative)) {
            ++lost;
            continue;
        }
        lines.append(relative);
    }
    if (dropped)
        *dropped = lost;
    if (lost > 0) {
        gaps.append({QString(), QStringLiteral("%1 name(s) hold a newline, which a list of "
                                               "one path to a line cannot hold; they were "
                                               "not listed")
                                    .arg(lost)});
    }
    // Sorted here rather than trusted from the caller: everything that reads
    // this is allowed to assume the order, and a walk's order is the
    // filesystem's business.
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

    QByteArray body;
    // A gap's path and sentence may hold a newline, or a space, of their own.
    for (const Gap &gap : std::as_const(gaps)) {
        body += '/';
        body += QUrl::toPercentEncoding(gap.path, "/");
        body += ' ';
        body += QString(gap.said).replace(QLatin1Char('\n'), QLatin1String("\\n")).toUtf8();
        body += '\n';
    }
    for (const QString &line : std::as_const(lines)) {
        body += line.toUtf8();
        body += '\n';
    }
    // By rename, the way the index and the roots are written. A cache is
    // disposable and a half-written one is not: a reader that caught this file
    // mid-write would answer out of an arbitrary prefix of the tree and call it
    // the whole answer.
    QString why;
    if (!writeKeptFile(path, body, &why)) {
        if (error)
            *error = QStringLiteral("names: cannot write %1: %2").arg(spelled(path), why);
        return false;
    }
    return true;
}

bool readNames(const Root &root, QStringList *paths, qint64 *ageSeconds, QString *error,
               QVector<Gap> *gaps, const Store *rows)
{
    if (paths)
        paths->clear();
    if (gaps)
        gaps->clear();
    if (ageSeconds)
        *ageSeconds = -1;

    const QString path = namesCachePath(root.id);
    QFile file(path);
    QString why;
    const PathState opened = openToRead(&file, {}, &why);
    // Not an error, and it must not be: it is a root nothing has swept yet, and
    // the caller's job is to say that rather than to answer out of nothing.
    if (opened == PathState::Missing)
        return true;
    if (opened != PathState::Present) {
        if (error)
            *error = QStringLiteral("names: cannot read %1").arg(why);
        return false;
    }
    if (ageSeconds) {
        const QDateTime built = QFileInfo(path).lastModified();
        // A list stamped in the future is new, not missing: -1 is "no file".
        *ageSeconds = built.isValid()
            ? qMax<qint64>(0, built.secsTo(QDateTime::currentDateTime()))
            : -1;
    }
    if (!paths && !gaps)
        return true;

    const QString base = root.path.endsWith(QLatin1Char('/'))
        ? root.path
        : root.path + QLatin1Char('/');
    QSet<QString> rowFolds;
    if (rows && paths) {
        for (const Record &record : rows->records())
            rowFolds.insert(foldedName(record.path));
    }
    Spellings known;
    QSet<QString> seen;
    for (int number = 1; !file.atEnd(); ++number) {
        QByteArray line = file.readLine();
        if (line.endsWith('\n'))
            line.chop(1);
        QString text;
        // A line `writeNames` did not write -- bytes that are not UTF-8, a path
        // that is empty, `.`, not clean or climbs out of the root, or one listed
        // already -- would be printed as a file under it that is somewhere else,
        // or twice.
        const bool read = nameFromBytes(line, &text);
        const bool gap = text.startsWith(QLatin1Char('/'));
        const qsizetype space = gap ? text.indexOf(QLatin1Char(' ')) : -1;
        const bool clean = gap ? space >= 0
                               : isCleanRelative(text) && text != QLatin1String(".");
        const bool repeated = clean && !gap && seen.contains(text);
        if (!read || !clean || repeated) {
            if (error)
                *error = QStringLiteral("names: %1:%2 %3; `omafiles sweep` rebuilds the list")
                             .arg(spelled(path))
                             .arg(number)
                             .arg(!read    ? QStringLiteral("is not valid UTF-8")
                                  : !clean ? QStringLiteral("is not a clean path inside the root")
                                           : QStringLiteral("repeats a path listed above"));
            if (paths)
                paths->clear();
            if (gaps)
                gaps->clear();
            return false;
        }
        if (gap) {
            if (gaps) {
                gaps->append({QUrl::fromPercentEncoding(text.mid(1, space - 1).toUtf8()),
                              text.mid(space + 1)});
            }
        } else {
            seen.insert(text);
            if (!paths)
                continue;
            const QString listed = base + text;
            paths->append(!rowFolds.isEmpty() && !rows->contains(listed)
                                  && rowFolds.contains(foldedName(listed))
                              ? storedSpelling(listed, &known)
                              : listed);
        }
    }
    return true;
}

QString spelledDuration(qint64 seconds)
{
    if (seconds >= 86400)
        return QStringLiteral("%1d").arg(seconds / 86400);
    if (seconds >= 3600)
        return QStringLiteral("%1h").arg(seconds / 3600);
    if (seconds >= 60)
        return QStringLiteral("%1m").arg(seconds / 60);
    return QStringLiteral("%1s").arg(qMax<qint64>(seconds, 0));
}

bool forgetNames(const QString &rootId, QString *error)
{
    const QString path = namesCachePath(rootId);
    if (::unlink(QFile::encodeName(path).constData()) == 0 || errno == ENOENT)
        return true;
    *error = QStringLiteral("cannot remove %1: %2").arg(spelled(path), systemReason(errno));
    return false;
}

} // namespace omafiles
