#pragma once

#include <QDBusContext>
#include <QObject>
#include <QStringList>

class QWindow;

namespace omafiles {
class Workspace;

struct OpenLocation {
    QString directory;
    QString cursor;
};

class FileManager : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.FileManager1")
public:
    enum Mode { Automatic, Folders, Items, Properties };
    explicit FileManager(QObject *parent = nullptr) : QObject(parent) {}
    static bool resolve(const QStringList &arguments, Mode mode,
                        QList<OpenLocation> *locations, QString *error);
    void setWorkspace(Workspace *workspace) { m_workspace = workspace; }
    void setWindow(QWindow *window);
    static QString launchToken();
    static void present(QWindow *window, const QString &startupId);
    void open(const QList<OpenLocation> &locations, bool reuseInitial = false);

public slots:
    QString Open(const QStringList &uris, const QString &startupId);
    void ShowFolders(const QStringList &uris, const QString &startupId);
    void ShowItems(const QStringList &uris, const QString &startupId);
    void ShowItemProperties(const QStringList &uris, const QString &startupId);

private:
    QString dispatch(const QStringList &uris, const QString &startupId, Mode mode);
    Workspace *m_workspace = nullptr;
};
} // namespace omafiles
