#pragma once

#include <QHash>
#include <QMultiHash>
#include <QObject>
#include <QSet>
#include <QSocketNotifier>
#include <QString>

namespace omafiles {

/// Watches files and directories through inotify, reporting a path as changed
/// and forgetting it once it no longer names the watched file. A directory
/// watch reports only its own entries: events about the entries themselves make
/// the kernel flag every cached child when the watch is added.
class EntryWatcher : public QObject
{
    Q_OBJECT
public:
    explicit EntryWatcher(QObject *parent = nullptr);
    ~EntryWatcher() override;
    bool addPath(const QString &path);
    void removePath(const QString &path);
    bool contains(const QString &path) const { return m_watches.contains(path); }

signals:
    void fileChanged(const QString &path);
    void directoryChanged(const QString &path);

private:
    void read();
    void forget(const QString &path);
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QHash<QString, int> m_watches;
    QMultiHash<int, QString> m_paths;
    QSet<QString> m_directories;
};

} // namespace omafiles
