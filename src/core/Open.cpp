#include "Open.h"

#include "Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

#include <utility>


namespace omafiles {

namespace {

QByteArray runQuery(const QString &program, const QStringList &args)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForFinished(3000)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    return proc.readAllStandardOutput().trimmed();
}

QString findExecutable(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

QString findDesktopFile(const QString &desktopId)
{
    if (desktopId.isEmpty())
        return {};
    const QStringList roots = {
        QDir::homePath() + QStringLiteral("/.local/share/applications"),
        QStringLiteral("/usr/local/share/applications"),
        QStringLiteral("/usr/share/applications"),
    };
    for (const QString &root : roots) {
        const QString path = QDir(root).filePath(desktopId);
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

bool desktopWantsTerminal(const QString &desktopPath)
{
    QFile file(desktopPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.startsWith(QLatin1String("Terminal="), Qt::CaseInsensitive)) {
            const QString value = line.section(QLatin1Char('='), 1).trimmed().toLower();
            return value == QLatin1String("true") || value == QLatin1String("1");
        }
    }
    return false;
}

bool isEditorDesktop(const QString &desktopId)
{
    const QString id = desktopId.toLower();
    return id.contains(QLatin1String("nvim")) || id.contains(QLatin1String("vim"))
        || id.contains(QLatin1String("helix")) || id.contains(QLatin1String("emacs"))
        || id.contains(QLatin1String("nano")) || id.contains(QLatin1String("micro"))
        || id.contains(QLatin1String("code")) || id.contains(QLatin1String("cursor"))
        || id.contains(QLatin1String("zed")) || id.contains(QLatin1String("sublime"));
}

QString queryMime(const QString &path)
{
    const QString xdgMime = findExecutable(QStringLiteral("xdg-mime"));
    if (xdgMime.isEmpty())
        return {};
    return QString::fromUtf8(runQuery(xdgMime, {QStringLiteral("query"), QStringLiteral("filetype"), path}));
}

QString queryDefaultDesktop(const QString &mime)
{
    if (mime.isEmpty())
        return {};
    const QString xdgMime = findExecutable(QStringLiteral("xdg-mime"));
    if (xdgMime.isEmpty())
        return {};
    return QString::fromUtf8(runQuery(xdgMime, {QStringLiteral("query"), QStringLiteral("default"), mime}));
}

} // namespace


QJsonObject OpenPlan::toJson() const
{
    QJsonArray args;
    for (const QString &arg : arguments)
        args.append(arg);
    return QJsonObject{
        {QStringLiteral("program"), program},
        {QStringLiteral("arguments"), args},
        {QStringLiteral("mime"), mime},
        {QStringLiteral("desktop"), desktopId},
    };
}

QString formatOpenPlan(const OpenPlan &plan)
{
    QStringList parts;
    parts.append(plan.program);
    parts.append(plan.arguments);
    return parts.join(QLatin1Char(' '));
}

bool planOpen(const QString &path, OpenPlan *plan, QString *error, PathState *state)
{
    if (state)
        *state = PathState::Present;
    if (!plan) {
        if (error)
            *error = QStringLiteral("open: missing plan");
        return false;
    }
    const QString resolved = canonicalPath(path, error, state);
    if (resolved.isEmpty())
        return false;

    plan->mime = queryMime(resolved);
    plan->desktopId = queryDefaultDesktop(plan->mime);

    const QString desktopPath = findDesktopFile(plan->desktopId);
    const bool terminalApp = desktopWantsTerminal(desktopPath);
    const QString omarchyEditor = findExecutable(QStringLiteral("omarchy-launch-editor"));
    const QString xdgOpen = findExecutable(QStringLiteral("xdg-open"));
    const QString gio = findExecutable(QStringLiteral("gio"));

    if (terminalApp && !omarchyEditor.isEmpty() && isEditorDesktop(plan->desktopId)) {
        plan->program = omarchyEditor;
        plan->arguments = {resolved};
        return true;
    }

    if (!xdgOpen.isEmpty()) {
        plan->program = xdgOpen;
        plan->arguments = {resolved};
        return true;
    }

    if (!gio.isEmpty()) {
        plan->program = gio;
        plan->arguments = {QStringLiteral("open"), resolved};
        return true;
    }

    if (error)
        *error = QStringLiteral("open: no xdg-open (needed for Omarchy mimeapps)");
    return false;
}

bool canShareLocalSend()
{
    return !findExecutable(QStringLiteral("localsend")).isEmpty()
        || !findExecutable(QStringLiteral("omarchy-menu-share")).isEmpty();
}

namespace {

// The desktop's own answer to a request: what open and share did when they were
// the only thing they could do.
bool desktopOpen(const QString &path, QString *error)
{
    OpenPlan plan;
    if (!planOpen(path, &plan, error))
        return false;
    if (!QProcess::startDetached(plan.program, plan.arguments)) {
        if (error)
            *error = QStringLiteral("open failed: %1").arg(formatOpenPlan(plan));
        return false;
    }
    return true;
}

bool desktopShare(const QString &path, QString *error)
{
    const QString omarchyShare = findExecutable(QStringLiteral("omarchy-menu-share"));
    if (!omarchyShare.isEmpty()) {
        const QString mode = QFileInfo(path).isDir() ? QStringLiteral("folder")
                                                      : QStringLiteral("file");
        if (!QProcess::startDetached(omarchyShare, {mode, path})) {
            if (error)
                *error = QStringLiteral("share failed: omarchy-menu-share %1 %2")
                             .arg(mode, path);
            return false;
        }
        return true;
    }


    const QString localsend = findExecutable(QStringLiteral("localsend"));
    const QString systemdRun = findExecutable(QStringLiteral("systemd-run"));

    if (!systemdRun.isEmpty() && !localsend.isEmpty()) {
        if (!QProcess::startDetached(systemdRun,
                                     {QStringLiteral("--user"),
                                      QStringLiteral("--quiet"),
                                      QStringLiteral("--collect"),
                                      QStringLiteral("localsend"),
                                      QStringLiteral("--headless"),
                                      QStringLiteral("send"),
                                      path})) {
            if (error)
                *error = QStringLiteral("share failed: systemd-run localsend send %1").arg(path);
            return false;
        }
        return true;
    }

    if (!localsend.isEmpty()) {
        if (!QProcess::startDetached(localsend,
                                     {QStringLiteral("--headless"),
                                      QStringLiteral("send"),
                                      path})) {
            if (error)
                *error = QStringLiteral("share failed: localsend send %1").arg(path);
            return false;
        }
        return true;
    }

    if (error)
        *error = QStringLiteral("share: LocalSend not found");
    return false;
}

bool desktopLauncher(const OpenRequest &request, QString *error)
{
    return request.kind == OpenRequest::Share ? desktopShare(request.path, error)
                                              : desktopOpen(request.path, error);
}

// A function-local static rather than a namespace-scope one: the studio test
// binary installs its recorder from a static initialiser, before `main`, and a
// namespace-scope launcher in this translation unit might not have been
// constructed yet when it did.
Launcher &installedLauncher()
{
    static Launcher launcher = desktopLauncher;
    return launcher;
}

} // namespace

Launcher setLauncher(Launcher launcher)
{
    Launcher previous = installedLauncher();
    installedLauncher() = launcher ? std::move(launcher) : Launcher(desktopLauncher);
    return previous;
}

bool openLocalPath(const QString &path, QString *error)
{
    return installedLauncher()(OpenRequest{OpenRequest::Open, path}, error);
}

bool shareLocalPath(const QString &path, QString *error)
{
    return installedLauncher()(OpenRequest{OpenRequest::Share, path}, error);
}

} // namespace omafiles
