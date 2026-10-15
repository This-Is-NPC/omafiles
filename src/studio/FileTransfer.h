#pragma once

#include <QObject>
#include <QQuickItem>
#include <QStringList>
#include <QUrl>
#include <QtQmlIntegration/qqmlintegration.h>
#include <functional>

class QDrag;

namespace omafiles {

using FileDragRunner = std::function<Qt::DropAction(QDrag *)>;
FileDragRunner setFileDragRunner(FileDragRunner runner);

Qt::DropAction fileDropAction(bool sameDevice, Qt::KeyboardModifiers modifiers);

class FileTransfer : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    using QObject::QObject;
    Q_INVOKABLE void startDrag(QQuickItem *source, const QStringList &paths);
    Q_INVOKABLE bool receive(const QList<QUrl> &urls, const QString &directory,
                            int modifiers, int possibleActions);

signals:
    void requested(const QStringList &paths, const QString &directory, int action);
    void refused(const QString &reason);
};

class FileDropTarget : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString directory READ directory WRITE setDirectory NOTIFY directoryChanged)
    Q_PROPERTY(bool containsDrag READ containsDrag NOTIFY containsDragChanged)

public:
    explicit FileDropTarget(QQuickItem *parent = nullptr);
    QString directory() const { return m_directory; }
    void setDirectory(const QString &directory);
    bool containsDrag() const { return m_containsDrag; }

signals:
    void directoryChanged();
    void containsDragChanged();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void setContainsDrag(bool contains);
    QString m_directory;
    bool m_containsDrag = false;
};

} // namespace omafiles
