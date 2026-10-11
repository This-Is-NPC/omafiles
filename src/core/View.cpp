#include "View.h"

#include "Paths.h"
#include "Store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace omafiles {

QString defaultViewsPath()
{
    return QDir(configHome()).filePath(QStringLiteral("omafiles/views.jsonl"));
}

bool isValidViewName(const QString &name)
{
    if (name.isEmpty() || name.size() > 64)
        return false;
    for (const QChar &letter : name) {
        const bool ordinary = letter.isLetterOrNumber() || letter == QLatin1Char('-')
            || letter == QLatin1Char('_') || letter == QLatin1Char(' ')
            || letter == QLatin1Char('.');
        if (!ordinary)
            return false;
    }
    return true;
}

QString normalizeViewName(const QString &raw, QString *error)
{
    qsizetype start = 0;
    qsizetype end = raw.size();
    while (start < end && raw.at(start) == u' ')
        ++start;
    while (end > start && raw.at(end - 1) == u' ')
        --end;
    const QString name = raw.mid(start, end - start);
    if (!isValidViewName(name)) {
        if (error) {
            *error = QStringLiteral("%1 is not a name; letters, digits, spaces, "
                                    "`-`, `_` and `.`, at most 64")
                         .arg(spelled(raw));
        }
        return {};
    }
    return name;
}

bool loadViews(const QString &path, QVector<View> *views, QString *error)
{
    views->clear();
    QVector<KeptLine> lines;
    QString why;
    if (!readKeptLines(path, QStringLiteral("views"), &lines, &why)) {
        if (error)
            *error = why;
        return false;
    }
    for (const KeptLine &line : std::as_const(lines)) {
        const QJsonObject &object = line.object;
        const QJsonValue name = object.value(QStringLiteral("name"));
        const QJsonValue dir = object.value(QStringLiteral("dir"));
        const QJsonValue tags = object.value(QStringLiteral("tags"));
        View view{name.toString(), dir.toString(), {}};
        // The shape `view add` writes, and nothing it would have refused.
        const auto problem = [&]() -> QString {
            if (!name.isString() || normalizeViewName(view.name, nullptr) != view.name)
                return QStringLiteral("it has no usable name");
            // A dir that is no path is named before the pair it is in.
            if (!dir.isUndefined() && !isCleanAbsolute(view.dir))
                return QStringLiteral("its dir is not an absolute path in its clean form");
            if (dir.isUndefined() == tags.isUndefined())
                return QStringLiteral("it needs a dir or tags, and not both");
            if (const QString shape = tags.isUndefined() ? QString() : keptTagsProblem(tags);
                !shape.isEmpty())
                return shape;
            for (const View &already : std::as_const(*views)) {
                if (already.name.compare(view.name, Qt::CaseInsensitive) == 0)
                    return QStringLiteral("%1 is on an earlier line too").arg(view.name);
            }
            return {};
        };
        why = problem();
        if (!why.isEmpty()) {
            if (error)
                *error = QStringLiteral("views: malformed jsonl at %1:%2: %3")
                             .arg(spelled(path), QString::number(line.number), why);
            return false;
        }
        for (const QJsonValue &tag : tags.toArray())
            view.tags.append(tag.toString());
        views->append(view);
    }
    return true;
}

bool loadViews(QVector<View> *views, QString *error)
{
    return loadViews(defaultViewsPath(), views, error);
}

bool saveViews(const QString &path, const QVector<View> &views, QString *error)
{
    QVector<QJsonObject> lines;
    for (const View &view : views) {
        QJsonObject object{{QStringLiteral("name"), view.name}};
        if (!view.dir.isEmpty())
            object.insert(QStringLiteral("dir"), view.dir);
        if (!view.tags.isEmpty()) {
            QJsonArray tags;
            for (const QString &tag : view.tags)
                tags.append(tag);
            object.insert(QStringLiteral("tags"), tags);
        }
        lines.append(object);
    }
    QString why;
    if (!writeKeptLines(path, lines, &why)) {
        if (error)
            *error = QStringLiteral("views: cannot write %1: %2").arg(spelled(path), why);
        return false;
    }
    return true;
}

} // namespace omafiles
