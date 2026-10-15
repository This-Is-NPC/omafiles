#include "FileManager.h"
#include "Paths.h"
#include "FileMetadata.h"
#include "Workspace.h"

#include <QDateTime>
#include <QFileInfo>
#include <QUrl>
#include <QWindow>

namespace omafiles {
QString FileManager::launchToken()
{
    const QByteArray token = qgetenv("XDG_ACTIVATION_TOKEN");
    return QString::fromUtf8(token.isEmpty() ? qgetenv("DESKTOP_STARTUP_ID") : token);
}

void FileManager::present(QWindow *window, const QString &startupId)
{
    // Qt's Wayland shell consumes this token during show/requestActivate.
    if (!startupId.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", startupId.toUtf8());
    window->show();
    window->raise();
    window->requestActivate();
    if (!startupId.isEmpty())
        qunsetenv("XDG_ACTIVATION_TOKEN");
}

void FileManager::setWindow(QWindow *window)
{
    connect(m_workspace, &Workspace::presentationRequested, window,
            [window](const QString &startupId) { present(window, startupId); });
}

bool FileManager::resolve(const QStringList &arguments, Mode mode,
                          QList<OpenLocation> *locations, QString *error)
{
    QList<OpenLocation> result;
    for (const QString &argument : arguments) {
        const bool isUri = argument.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)
                           || argument.contains(QLatin1String("://"));
        const QUrl url(argument, QUrl::StrictMode);
        const QString typed = isUri ? localFileOf(url) : argument;
        if (typed.isEmpty() || (isUri && !url.isValid())) {
            *error = QStringLiteral("invalid local file URI: %1").arg(spelled(argument));
            return false;
        }
        // Resolve the parent separately so revealing a symlink selects its own name.
        const QString path = canonicalLinkPath(typed, error);
        if (path.isEmpty())
            return false;
        const QFileInfo info(path);
        if (mode == Folders && !info.isDir()) {
            *error = QStringLiteral("%1 is not a folder").arg(spelled(path));
            return false;
        }
        const bool reveal = mode == Items || mode == Properties || !info.isDir();
        result.append({reveal ? typedDirectory(path, nullptr) : path, reveal ? path : QString()});
    }
    *locations = result;
    return true;
}

void FileManager::open(const QList<OpenLocation> &locations, bool reuseInitial)
{
    for (const auto &location : locations) {
        if (reuseInitial) {
            m_workspace->currentTab()->setLocation(location.directory, location.cursor);
            reuseInitial = false;
        } else {
            m_workspace->paneAtIndex(m_workspace->focusedPane())
                ->addTab(QStringLiteral("files"), location.directory, location.cursor);
        }
    }
}

QString FileManager::dispatch(const QStringList &uris, const QString &startupId, Mode mode)
{
    QList<OpenLocation> locations;
    QString error;
    if (!resolve(uris, mode, &locations, &error)) {
        if (calledFromDBus() && mode != Automatic)
            sendErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), error);
        return error;
    }
    if (mode == Properties) {
        QStringList descriptions;
        for (const auto &location : locations) {
            const QFileInfo info(location.cursor);
            descriptions.append(QStringLiteral("%1\nSize: %2 bytes\nOwner: %3\nPermissions: %4\nModified: %5")
                .arg(info.filePath()).arg(info.size()).arg(info.owner())
                .arg(modeOf(info)).arg(info.lastModified().toString(Qt::ISODate)));
        }
        if (!descriptions.isEmpty())
            emit m_workspace->propertiesRequested(descriptions.join(QStringLiteral("\n\n")));
    } else {
        open(locations);
    }
    emit m_workspace->presentationRequested(startupId);
    return {};
}

QString FileManager::Open(const QStringList &uris, const QString &startupId)
{
    return dispatch(uris, startupId, Automatic);
}
void FileManager::ShowFolders(const QStringList &uris, const QString &startupId)
{
    dispatch(uris, startupId, Folders);
}
void FileManager::ShowItems(const QStringList &uris, const QString &startupId)
{
    dispatch(uris, startupId, Items);
}
void FileManager::ShowItemProperties(const QStringList &uris, const QString &startupId)
{
    dispatch(uris, startupId, Properties);
}
} // namespace omafiles
