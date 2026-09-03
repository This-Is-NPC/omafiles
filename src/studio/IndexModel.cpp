#include "IndexModel.h"

#include "Open.h"
#include "Paths.h"

#include <QDir>
#include <QFileInfo>

namespace omafiles {

class TagListModel : public QAbstractListModel
{
public:
    enum Roles { TagRole = Qt::UserRole + 1, CountRole };
    explicit TagListModel(IndexModel *owner)
        : QAbstractListModel(owner)
        , m_owner(owner)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return m_tags.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_tags.size())
            return {};
        const auto &row = m_tags.at(index.row());
        if (role == TagRole || role == Qt::DisplayRole)
            return row.tag;
        if (role == CountRole)
            return row.count;
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{TagRole, "tag"}, {CountRole, "count"}};
    }

    void rebuild()
    {
        beginResetModel();
        QMap<QString, int> counts;
        for (const Record &record : m_owner->records()) {
            if (!statable(record.path, nullptr))
                continue;
            for (const QString &tag : record.tags)
                counts[tag] += 1;
        }
        m_tags.clear();
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            m_tags.push_back({it.key(), it.value()});
        endResetModel();
    }

private:
    struct Row {
        QString tag;
        int count = 0;
    };
    IndexModel *m_owner;
    QVector<Row> m_tags;
};

IndexModel::IndexModel(QObject *parent)
    : QObject(parent)
    , m_store(defaultIndexPath())
{
    m_tags = new TagListModel(this);
    connect(this, &IndexModel::changed, m_tags, &TagListModel::rebuild);
    reload();

    // The directory has to exist before it can be watched, and on a machine
    // where the command line has never run, it does not. Both watches are taken
    // once, here, so without this a fresh install watches nothing and the
    // window does not follow the index until somebody presses F5. The studio
    // owns this directory as much as the CLI does.
    const QFileInfo info(m_store.indexPath());
    info.dir().mkpath(QStringLiteral("."));
    if (info.dir().exists())
        m_watcher.addPath(info.absolutePath());
    if (QFileInfo::exists(m_store.indexPath()))
        m_watcher.addPath(m_store.indexPath());

    m_settle.setSingleShot(true);
    m_settle.setInterval(60);
    connect(&m_settle, &QTimer::timeout, this, &IndexModel::reload);
    const auto later = [this] { m_settle.start(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, later);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, later);
}

int IndexModel::tagCount() const
{
    return m_tags->rowCount();
}

QAbstractListModel *IndexModel::tagsModel() const
{
    return m_tags;
}

QStringList IndexModel::tagsForPath(const QString &path) const
{
    return m_store.get(path).tags;
}

void IndexModel::reload()
{
    QString error;
    const bool loaded = m_store.load(&error);

    // Tallied on every read, not only when the index is written: what the
    // system says about a row changes when a drive is mounted or a permission
    // is set, and the index does not move for either.
    m_present = 0;
    m_missing = 0;
    m_unknown = 0;
    QString declined;
    for (const Record &record : m_store.records()) {
        QString detail;
        switch (pathState(record.path, &detail)) {
        case PathState::Present:
            ++m_present;
            break;
        case PathState::Missing:
            ++m_missing;
            break;
        case PathState::Unknown:
            ++m_unknown;
            if (declined.isEmpty())
                declined = detail;
            break;
        }
    }

    // The counts are of rows the system will describe. A count that came up
    // short is not the answer that was asked for, so it says so -- the CLI
    // spends an exit code on this and the window has only this line. A row that
    // is simply gone is not worth a permanent alarm on its own, but an index
    // with rows in it and nothing reachable is: the zeros above are a lie about
    // an index that is not empty.
    if (!loaded)
        m_error = error;
    else if (m_unknown > 0)
        m_error = QStringLiteral("%1 index row(s) not counted; %2").arg(m_unknown).arg(declined);
    else if (m_missing > 0 && m_present == 0)
        m_error = QStringLiteral("%1 index row(s), and none of their files are there; "
                                 "omafiles prune drops them").arg(m_missing);
    else
        m_error.clear();
    // A commit lands by rename, which takes the watch on the old file with it,
    // so the file watch is re-taken on every read rather than only at startup.
    if (QFileInfo::exists(m_store.indexPath())
        && !m_watcher.files().contains(m_store.indexPath()))
        m_watcher.addPath(m_store.indexPath());
    emit changed();
}

void IndexModel::openPath(const QString &path)
{
    QString error;
    if (!openLocalPath(path, &error)) {
        m_error = error;
        emit changed();
    }
}

bool IndexModel::canShare() const
{
    return canShareLocalSend();
}

void IndexModel::sharePath(const QString &path)
{
    QString error;
    if (!shareLocalPath(path, &error)) {
        m_error = error;
        emit changed();
    }
}

QString IndexModel::tagAt(int row) const
{
    if (!m_tags || row < 0 || row >= m_tags->rowCount())
        return {};
    return m_tags->data(m_tags->index(row, 0), TagListModel::TagRole).toString();
}

int IndexModel::tagRowCount() const
{
    return m_tags ? m_tags->rowCount() : 0;
}

} // namespace omafiles
