#include "FileTransfer.h"
#include "BlockingJob.h"

#include "Clipboard.h"
#include "Paths.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QQmlEngine>
#include <QFile>
#include <QFutureWatcher>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <sys/stat.h>
#include <cerrno>
#include <utility>

namespace omafiles {
namespace {
FileDragRunner dragRunner;

struct DropPlan {
    Qt::DropAction action = Qt::IgnoreAction;
    QString error;
};

DropPlan planDrop(const QStringList &paths, const QString &directory,
                  Qt::KeyboardModifiers modifiers, Qt::DropActions possible)
{
    struct stat destination {};
    if (::stat(QFile::encodeName(directory).constData(), &destination) != 0)
        return {Qt::IgnoreAction, QStringLiteral("Cannot drop into %1: %2")
                    .arg(directory, systemReason(errno))};
    if (!S_ISDIR(destination.st_mode))
        return {Qt::IgnoreAction, QStringLiteral("Cannot drop: %1 is not a folder").arg(directory)};
    bool same = true;
    for (const QString &path : paths) {
        struct stat source {};
        if (::lstat(QFile::encodeName(path).constData(), &source) != 0)
            return {Qt::IgnoreAction, QStringLiteral("Cannot drop %1: %2")
                        .arg(path, systemReason(errno))};
        same &= source.st_dev == destination.st_dev;
    }
    return {fileDropAction(same, modifiers, possible), {}};
}
} // namespace

FileDragRunner setFileDragRunner(FileDragRunner runner)
{
    return std::exchange(dragRunner, std::move(runner));
}

// An action the source does not offer becomes a copy, as QDropEvent::setDropAction does.
Qt::DropAction fileDropAction(bool sameDevice, Qt::KeyboardModifiers modifiers,
                              Qt::DropActions possible)
{
    const bool copy = modifiers.testFlag(Qt::ControlModifier);
    const bool move = modifiers.testFlag(Qt::ShiftModifier);
    const Qt::DropAction action = copy && move ? Qt::LinkAction
        : copy ? Qt::CopyAction
        : move || sameDevice ? Qt::MoveAction
        : Qt::CopyAction;
    return possible.testFlag(action) ? action : Qt::CopyAction;
}

void FileTransfer::startDrag(QQuickItem *source, const QStringList &paths)
{
    if (!source || !source->window() || paths.isEmpty())
        return;
    // A delegate can disappear during the native drag's nested event loop.
    QPointer<QDrag> drag = new QDrag(source->window());
    drag->setMimeData(fileMimeData(paths, false));
    const auto runner = dragRunner;
    if (runner)
        runner(drag);
    else
        drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
    if (drag)
        drag->deleteLater();
}

bool FileTransfer::receive(const QList<QUrl> &urls, const QString &directory,
                            int modifiers, int possibleActions)
{
    // Copy transport prevents a foreign source deleting files before our
    // asynchronous operation succeeds. The destination owns the whole move.
    if (!(possibleActions & Qt::CopyAction)) {
        emit refused(QStringLiteral("Cannot drop: the source must offer file copying"));
        return false;
    }
    if (urls.isEmpty() || directory.isEmpty())
        return false;
    const QStringList paths = localFilePaths(urls);
    if (paths.isEmpty()) {
        emit refused(QStringLiteral("Cannot drop: every URL must name a local file"));
        return false;
    }
    auto *watcher = new QFutureWatcher<DropPlan>(this);
    connect(watcher, &QFutureWatcher<DropPlan>::finished, this,
            [this, watcher, paths, directory] {
        const auto plan = watcher->result();
        watcher->deleteLater();
        if (!plan.error.isEmpty())
            emit refused(plan.error);
        else
            emit requested(paths, directory, plan.action);
    });
    watcher->setFuture(runBlocking([paths, directory, modifiers, possibleActions] {
        return planDrop(paths, directory, Qt::KeyboardModifiers(modifiers),
                        Qt::DropActions(possibleActions));
    }));
    return true;
}

FileDropTarget::FileDropTarget(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemAcceptsDrops, true);
}

void FileDropTarget::setDirectory(const QString &directory)
{
    if (m_directory == directory)
        return;
    m_directory = directory;
    emit directoryChanged();
}

void FileDropTarget::setContainsDrag(bool contains)
{
    if (m_containsDrag == contains)
        return;
    m_containsDrag = contains;
    emit containsDragChanged();
}

void FileDropTarget::dragEnterEvent(QDragEnterEvent *event)
{
    dragMoveEvent(event);
}

void FileDropTarget::dragMoveEvent(QDragMoveEvent *event)
{
    const bool accepted = !m_directory.isEmpty() && isEnabled()
        && event->mimeData()->hasUrls() && event->possibleActions().testFlag(Qt::CopyAction);
    setContainsDrag(accepted);
    event->setDropAction(Qt::CopyAction);
    event->setAccepted(accepted);
}

void FileDropTarget::dragLeaveEvent(QDragLeaveEvent *)
{
    setContainsDrag(false);
}

void FileDropTarget::dropEvent(QDropEvent *event)
{
    setContainsDrag(false);
    event->ignore();
    auto *engine = qmlEngine(this);
    if (!engine || m_directory.isEmpty() || !isEnabled())
        return;
    auto *transfer = engine->singletonInstance<FileTransfer *>(QStringLiteral("omafiles"),
                                                               QStringLiteral("FileTransfer"));
    if (transfer && transfer->receive(event->mimeData()->urls(), m_directory,
                                     event->modifiers(), event->possibleActions())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

} // namespace omafiles
