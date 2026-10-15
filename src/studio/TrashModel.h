#pragma once

#include "Trash.h"
#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {

class TrashModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int markedCount READ markedCount NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
public:
    enum Role { NameRole = Qt::UserRole + 1, PathRole, OriginalRole, DeletedRole, ErrorRole, MarkedRole };
    explicit TrashModel(QObject *parent = nullptr);
    ~TrashModel() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return m_items.size(); }
    int markedCount() const { return m_marks.size(); }
    bool loading() const { return m_loading; }
    bool busy() const { return m_busy; }
    QString error() const { return m_error; }
    QString message() const { return m_message; }
    Q_INVOKABLE QString details(int row) const;
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE int rowForPath(const QString &path) const;
    Q_INVOKABLE void reload();
    Q_INVOKABLE void toggleMark(int row);
    Q_INVOKABLE void markAll();
    Q_INVOKABLE void clearMarks();
    Q_INVOKABLE void restore(int row);
    Q_INVOKABLE bool prepareEmpty();
    Q_INVOKABLE void empty();
signals:
    void changed();
    void operationFinished();
private:
    void operate(const QVector<Trash::Item> &items, bool erase);
    QVector<Trash::Item> m_items;
    QVector<Trash::Item> m_emptySnapshot;
    QSet<QString> m_marks;
    QString m_message;
    QString m_receipt;
    QString m_error;
    bool m_loading = false;
    bool m_busy = false;
    bool m_receiptPending = false;
    std::shared_ptr<std::atomic_bool> m_scan;
    std::shared_ptr<std::atomic_bool> m_operation;
    QFileSystemWatcher m_watcher;
    QTimer m_settle;
};
} // namespace omafiles
