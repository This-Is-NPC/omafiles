#include "EntryWatcher.h"

#include <QFile>
#include <QList>

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omafiles {

EntryWatcher::EntryWatcher(QObject *parent)
    : QObject(parent), m_fd(inotify_init1(IN_NONBLOCK | IN_CLOEXEC))
{
    if (m_fd < 0)
        return;
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &EntryWatcher::read);
}

EntryWatcher::~EntryWatcher()
{
    delete m_notifier;
    if (m_fd >= 0)
        ::close(m_fd);
}

bool EntryWatcher::addPath(const QString &path)
{
    if (m_watches.contains(path))
        return true;
    const QByteArray native = QFile::encodeName(path);
    struct stat info {};
    if (m_fd < 0 || ::stat(native.constData(), &info) != 0)
        return false;
    const bool directory = S_ISDIR(info.st_mode);
    const int descriptor = inotify_add_watch(m_fd, native.constData(), directory
        ? IN_CREATE | IN_DELETE | IN_MOVE | IN_DELETE_SELF | IN_ONLYDIR
        : IN_ATTRIB | IN_MODIFY | IN_MOVE | IN_MOVE_SELF | IN_DELETE_SELF);
    if (descriptor < 0)
        return false;
    m_watches.insert(path, descriptor);
    m_paths.insert(descriptor, path);
    if (directory)
        m_directories.insert(path);
    return true;
}

void EntryWatcher::removePath(const QString &path)
{
    const int descriptor = m_watches.value(path, -1);
    if (descriptor < 0)
        return;
    forget(path);
    if (!m_paths.contains(descriptor))
        inotify_rm_watch(m_fd, descriptor);
}

void EntryWatcher::forget(const QString &path)
{
    m_paths.remove(m_watches.take(path), path);
    m_directories.remove(path);
}

void EntryWatcher::read()
{
    QList<std::pair<QString, bool>> changed;
    alignas(inotify_event) char buffer[4096];
    for (ssize_t size; (size = ::read(m_fd, buffer, sizeof buffer)) > 0;) {
        for (const char *at = buffer; at < buffer + size;) {
            const auto *event = reinterpret_cast<const inotify_event *>(at);
            at += sizeof(inotify_event) + event->len;
            const bool overflow = event->mask & IN_Q_OVERFLOW;
            const QStringList paths = overflow ? m_watches.keys() : m_paths.values(event->wd);
            for (const QString &path : paths) {
                const std::pair<QString, bool> entry{path, m_directories.contains(path)};
                if (!changed.contains(entry))
                    changed.append(entry);
                if (!overflow && event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF))
                    forget(path);
            }
            if (!overflow && event->mask & IN_MOVE_SELF && !m_paths.contains(event->wd))
                inotify_rm_watch(m_fd, event->wd);
        }
    }
    for (const auto &[path, directory] : changed) {
        if (directory)
            emit directoryChanged(path);
        else
            emit fileChanged(path);
    }
}

} // namespace omafiles
