#include "View.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

namespace omafiles {

QString defaultViewsPath()
{
    QString configHome = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    if (configHome.isEmpty())
        configHome = QDir::homePath() + QStringLiteral("/.config");
    return QDir(configHome).filePath(QStringLiteral("omafiles/views.jsonl"));
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
    const QString trimmed = raw.simplified();
    if (!isValidViewName(trimmed)) {
        if (error)
            *error = QStringLiteral("view: %1 is not a name; letters, digits, spaces, "
                                    "`-`, `_` and `.`, at most 64")
                         .arg(raw);
        return {};
    }
    return trimmed;
}

bool loadViews(const QString &path, QVector<View> *views, QString *error)
{
    if (!views)
        return false;
    views->clear();
    QFile file(path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("views: cannot read %1").arg(path);
        return false;
    }

    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral("views: malformed jsonl at %1:%2").arg(path).arg(lineNumber);
            return false;
        }
        const QJsonObject object = doc.object();
        View view;
        view.name = object.value(QStringLiteral("name")).toString();
        view.dir = object.value(QStringLiteral("dir")).toString();
        for (const QJsonValue &value : object.value(QStringLiteral("tags")).toArray()) {
            if (value.isString())
                view.tags.append(value.toString());
        }
        if (!isValidViewName(view.name)) {
            if (error)
                *error = QStringLiteral("views: %1:%2 has no usable name").arg(path).arg(lineNumber);
            return false;
        }
        if (view.dir.isEmpty() && view.tags.isEmpty()) {
            if (error)
                *error = QStringLiteral("views: %1:%2 asks nothing; it needs a dir or a tag")
                             .arg(path).arg(lineNumber);
            return false;
        }
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
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("views: cannot write %1").arg(path);
        return false;
    }
    QTextStream out(&file);
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
        out << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)) << '\n';
    }
    out.flush();
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("views: cannot commit %1").arg(path);
        return false;
    }
    return true;
}

} // namespace omafiles
