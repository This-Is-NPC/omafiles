#pragma once

#include "View.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {

/// The saved views, as a list the panel can draw.
///
/// A singleton beside `IndexModel` rather than a corner of it: what a view
/// holds is a question somebody wrote down, and the index holds what the files
/// carry. They change for different reasons and are written to different files.
///
/// It watches its own file for the same reason the index does. `omafiles view
/// add` at a prompt and the entry in the row menu are the same command, so a
/// window that only noticed the second would disagree with the shell it was
/// started from.
class Views : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(int count READ rowCount NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        /// `folder` or `tags`: which of the two things this view saved.
        KindRole,
        DirRole,
        TagsRole,
        /// What it asks, as the command line that asks it.
        AsksRole
    };

    explicit Views(QObject *parent = nullptr);

    QString error() const { return m_error; }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void reload();
    /// The view on one row, as a map, for the caller that has to open it. Empty
    /// for a row that is not there, so QML can test it with one truthiness
    /// check rather than guarding every field.
    Q_INVOKABLE QVariantMap at(int row) const;

    /// Saves what is on screen under a name.
    ///
    /// Through `omafiles view add` and not by writing the file here, for the
    /// reason every other mutation in this window goes that way: the rules
    /// about what a name may hold, and what a view may ask, live in one place
    /// and a second copy of them would drift.
    Q_INVOKABLE void saveFolder(const QString &name, const QString &directory);
    Q_INVOKABLE void saveQuery(const QString &name, const QStringList &tags);
    Q_INVOKABLE void forget(const QString &name);
    /// What the last save could not do, for the status bar.
    void say(const QString &message);

signals:
    void changed();

private:
    QVector<View> m_views;
    QString m_error;
    QFileSystemWatcher m_watcher;
    /// Coalesces the watcher, for the reason `IndexModel` gives about its own:
    /// one write is several filesystem events and the panel wants the state
    /// after them, once.
    QTimer m_settle;
};

} // namespace omafiles
