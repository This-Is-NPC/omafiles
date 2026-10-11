#include "Views.h"

#include "Paths.h"
#include "Verb.h"

#include <QFileInfo>
#include <QPointer>
#include <QVariantMap>

namespace omafiles {

Views::Views(QObject *parent)
    : QAbstractListModel(parent)
{
    reload();

    const QString folder = typedDirectory(defaultViewsPath(), nullptr);
    if (makePath(folder, nullptr))
        m_watcher.addPath(folder);
    if (QFileInfo::exists(defaultViewsPath()))
        m_watcher.addPath(defaultViewsPath());

    m_settle.setSingleShot(true);
    m_settle.setInterval(60);
    connect(&m_settle, &QTimer::timeout, this, &Views::reload);
    const auto later = [this] { m_settle.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, later);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, later);
}

void Views::reload()
{
    beginResetModel();
    m_error.clear();
    QVector<View> loaded;
    if (!loadViews(&loaded, &m_error))
        loaded.clear();
    m_views = loaded;
    endResetModel();

    // A save lands by rename, which takes the watch on the old file with it.
    const QString path = defaultViewsPath();
    if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
        m_watcher.addPath(path);
    emit changed();
}

int Views::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_views.size();
}

namespace {

QString asksOf(const View &view)
{
    QString asks = QStringLiteral("ls");
    for (const QString &tag : view.tags)
        asks += QStringLiteral(" --tag ") + tag;
    if (!view.dir.isEmpty())
        asks += QLatin1Char(' ') + view.dir;
    return asks;
}

} // namespace

QVariant Views::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_views.size())
        return {};
    const View &view = m_views.at(index.row());
    switch (role) {
    case NameRole:
    case Qt::DisplayRole:
        return view.name;
    case KindRole:
        return view.isFolder() ? QStringLiteral("folder") : QStringLiteral("tags");
    case DirRole:
        return view.dir;
    case TagsRole:
        return view.tags;
    case AsksRole:
        return asksOf(view);
    default:
        return {};
    }
}

QHash<int, QByteArray> Views::roleNames() const
{
    return {{NameRole, "name"},
            {KindRole, "kind"},
            {DirRole, "dir"},
            {TagsRole, "tags"},
            {AsksRole, "asks"}};
}

namespace {

void run(Views *self, const QStringList &arguments)
{
    QPointer<Views> alive(self);
    runVerb(arguments, [alive](bool ok, const QString &, const QString &error) {
        if (!alive)
            return;
        if (ok)
            alive->reload();
        else
            alive->say(error);
    });
}

} // namespace

void Views::saveFolder(const QString &name, const QString &directory)
{
    if (name.isEmpty() || directory.isEmpty())
        return;
    run(this, {QStringLiteral("view"), QStringLiteral("add"), QStringLiteral("--name"), name,
               QStringLiteral("--"), directory});
}

void Views::saveQuery(const QString &name, const QStringList &tags)
{
    if (name.isEmpty())
        return;
    // A view is a folder or a set of tags, and the complement of every tag is
    // neither. Saying so beats saving something that would open on the wrong
    // listing: `view` has no shape for it yet, and inventing one here would put
    // a row in the panel that the command line cannot make or read.
    if (tags.isEmpty()) {
        say(QStringLiteral("a view is a folder or a set of tags; "
                           "(no tag) is neither, and cannot be saved yet"));
        return;
    }
    QStringList arguments{QStringLiteral("view"), QStringLiteral("add"),
                          QStringLiteral("--name"), name};
    for (const QString &tag : tags)
        arguments << QStringLiteral("--tag") << tag;
    run(this, arguments);
}

void Views::forget(const QString &name)
{
    if (name.isEmpty())
        return;
    run(this, {QStringLiteral("view"), QStringLiteral("rm"), QStringLiteral("--name"), name});
}

void Views::say(const QString &message)
{
    m_error = message;
    emit changed();
}

QVariantMap Views::at(int row) const
{
    if (row < 0 || row >= m_views.size())
        return {};
    const View &view = m_views.at(row);
    return {{QStringLiteral("name"), view.name},
            {QStringLiteral("kind"), view.isFolder() ? QStringLiteral("folder")
                                                     : QStringLiteral("tags")},
            {QStringLiteral("dir"), view.dir},
            {QStringLiteral("tags"), view.tags},
            {QStringLiteral("asks"), asksOf(view)}};
}

} // namespace omafiles
