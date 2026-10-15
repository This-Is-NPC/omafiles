#include "TrashModel.h"
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <algorithm>
#include <tuple>

namespace omafiles {
namespace {
bool sameItems(const QVector<Trash::Item> &left, const QVector<Trash::Item> &right)
{
    const auto state = [](const Trash::Item &item) {
        return std::tie(item.path, item.info, item.original, item.deleted, item.error,
            item.location.path, item.location.top, item.metadata, item.signature,
            item.metadataSignature, item.device, item.inode, item.fileDevice, item.fileInode);
    };
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
        [&](const auto &a, const auto &b) { return state(a) == state(b); });
}
}

TrashModel::TrashModel(QObject *parent) : QAbstractListModel(parent)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(60);
    connect(&m_settle, &QTimer::timeout, this, &TrashModel::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_settle.start(); });
    QTimer::singleShot(0, this, &TrashModel::reload);
}

TrashModel::~TrashModel()
{
    if (m_scan) m_scan->store(true);
    if (m_operation) m_operation->store(true);
}

int TrashModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }

QVariant TrashModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return {};
    const auto &item = m_items.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole: return QFileInfo(item.original.isEmpty() ? item.path : item.original).fileName();
    case PathRole: return item.path;
    case OriginalRole: return item.original;
    case DeletedRole: return item.deleted;
    case ErrorRole: return item.error;
    case MarkedRole: return m_marks.contains(item.path);
    }
    return {};
}

QHash<int, QByteArray> TrashModel::roleNames() const
{
    return {{NameRole, "name"}, {PathRole, "path"}, {OriginalRole, "original"},
        {DeletedRole, "deleted"}, {ErrorRole, "problem"}, {MarkedRole, "marked"}};
}

QString TrashModel::pathAt(int row) const
{
    return row >= 0 && row < count() ? m_items.at(row).path : QString();
}

int TrashModel::rowForPath(const QString &path) const
{
    for (int row = 0; row < count(); ++row) if (m_items.at(row).path == path) return row;
    return -1;
}

QString TrashModel::details(int row) const
{
    if (row < 0 || row >= count()) return {};
    const auto &item = m_items.at(row);
    return QStringLiteral("Original: %1\nDeleted: %2\nStored: %3\n%4")
        .arg(item.original, item.deleted, item.path, item.error);
}

void TrashModel::reload()
{
    if (m_busy) return;
    if (m_scan) m_scan->store(true);
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_scan = cancel;
    m_loading = true;
    m_message = QStringLiteral("Refreshing Trash…");
    emit changed();
    auto *watcher = new QFutureWatcher<Trash::Listing>(this);
    connect(watcher, &QFutureWatcher<Trash::Listing>::finished, this, [this, watcher, cancel] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (cancel->load()) return;
        const QString error = result.errors.join(QLatin1Char('\n'));
        if (!m_receiptPending && (m_error != error || !sameItems(m_items, result.items)))
            m_receipt.clear();
        m_receiptPending = false;
        beginResetModel();
        m_items = result.items;
        QSet<QString> present;
        for (const auto &item : m_items) present.insert(item.path);
        m_marks.intersect(present);
        endResetModel();
        m_error = error;
        m_message = m_error.isEmpty() ? m_receipt : m_error;
        m_loading = false;
        if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
        if (!result.watches.isEmpty()) m_watcher.addPaths(result.watches);
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([cancel] { return Trash::list(Trash::mounts(), cancel); }));
}

void TrashModel::toggleMark(int row)
{
    if (row < 0 || row >= count() || m_busy) return;
    const auto path = m_items.at(row).path;
    if (m_marks.contains(path)) m_marks.remove(path); else m_marks.insert(path);
    emit dataChanged(index(row), index(row), {MarkedRole});
    emit changed();
}

void TrashModel::markAll()
{
    if (m_busy) return;
    for (const auto &item : m_items) m_marks.insert(item.path);
    if (count()) emit dataChanged(index(0), index(count() - 1), {MarkedRole});
    emit changed();
}

void TrashModel::clearMarks()
{
    m_marks.clear();
    if (count()) emit dataChanged(index(0), index(count() - 1), {MarkedRole});
    emit changed();
}

void TrashModel::restore(int row)
{
    QVector<Trash::Item> selected;
    if (!m_marks.isEmpty()) {
        for (const auto &item : m_items) if (m_marks.contains(item.path)) selected.append(item);
    } else if (row >= 0 && row < count()) selected.append(m_items.at(row));
    operate(selected, false);
}

bool TrashModel::prepareEmpty()
{
    if (m_busy || m_loading) return false;
    m_emptySnapshot = m_items;
    return !m_emptySnapshot.isEmpty();
}

void TrashModel::empty()
{
    const auto items = m_emptySnapshot;
    m_emptySnapshot.clear();
    operate(items, true);
}

void TrashModel::operate(const QVector<Trash::Item> &items, bool erase)
{
    if (m_busy || (!erase && m_loading) || items.isEmpty()) return;
    m_loading = false;
    if (m_scan) m_scan->store(true);
    m_busy = true;
    m_receipt.clear();
    m_receiptPending = false;
    m_message = erase ? QStringLiteral("Emptying Trash…") : QStringLiteral("Restoring…");
    emit changed();
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_operation = cancel;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        m_receipt = watcher->result();
        m_receiptPending = true;
        m_message = m_receipt;
        watcher->deleteLater();
        m_busy = false;
        emit changed();
        emit operationFinished();
        reload();
    });
    watcher->setFuture(QtConcurrent::run([items, erase, cancel] {
        int completed = 0;
        QStringList errors;
        for (const auto &item : items) {
            if (cancel->load()) break;
            const QString error = erase ? Trash::erase(item) : Trash::restore(item);
            if (error.isEmpty()) ++completed; else errors.append(error);
        }
        QString result = (erase ? QStringLiteral("Permanently deleted %1 item(s).")
                                : QStringLiteral("Restored %1 item(s).")).arg(completed);
        if (!errors.isEmpty()) result += QLatin1Char('\n') + errors.join(QLatin1Char('\n'));
        return result;
    }));
}
} // namespace omafiles
