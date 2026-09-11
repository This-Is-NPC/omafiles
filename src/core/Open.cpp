#include "Open.h"

#include "Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMimeDatabase>
#include <QMimeType>
#include <QProcess>
#include <QSet>
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

// The applications directories, in the order the desktop searches them: the
// user's own first, then the system's. `$XDG_DATA_HOME` and `$XDG_DATA_DIRS`
// rather than the three paths they usually mean, because a machine that sets
// them -- a Nix profile, a Flatpak export, a home built somewhere else -- keeps
// its applications where it says it does.
QStringList applicationDirs()
{
    const QByteArray home = qgetenv("XDG_DATA_HOME");
    const QByteArray dirs = qgetenv("XDG_DATA_DIRS");
    QStringList roots;
    roots.append(home.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share")
                                : QString::fromLocal8Bit(home));
    const QString rest = dirs.isEmpty() ? QStringLiteral("/usr/local/share:/usr/share")
                                        : QString::fromLocal8Bit(dirs);
    roots += rest.split(QLatin1Char(':'), Qt::SkipEmptyParts);

    QStringList out;
    for (const QString &root : std::as_const(roots)) {
        const QString path = QDir(root).filePath(QStringLiteral("applications"));
        if (!out.contains(path))
            out.append(path);
    }
    return out;
}

QString findDesktopFile(const QString &desktopId)
{
    if (desktopId.isEmpty())
        return {};
    const QStringList roots = applicationDirs();
    for (const QString &root : roots) {
        const QString path = QDir(root).filePath(desktopId);
        if (QFileInfo::exists(path))
            return path;
    }
    // A `-` in an id can stand for a directory separator, which is how
    // `kde-foo.desktop` names `applications/kde/foo.desktop`.
    QString nested = desktopId;
    while (true) {
        const int cut = nested.indexOf(QLatin1Char('-'));
        if (cut < 0)
            break;
        nested[cut] = QLatin1Char('/');
        for (const QString &root : roots) {
            const QString path = QDir(root).filePath(nested);
            if (QFileInfo::exists(path))
                return path;
        }
    }
    return {};
}

// One section of a desktop-entry file, as keys and values.
//
// Hand-read and not `QSettings`: this is not INI. A key here can carry a locale
// in brackets -- `Name[pt_BR]` -- which `QSettings` reads as the start of a
// group, and a value can hold the `;` and `\\` that INI escapes.
QMap<QString, QString> desktopSection(const QString &path, const QString &section)
{
    QMap<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;
    QTextStream stream(&file);
    bool inside = false;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            inside = line == QLatin1Char('[') + section + QLatin1Char(']');
            continue;
        }
        if (!inside || line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        values.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
    }
    return values;
}

QStringList desktopIds(const QString &value)
{
    return value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
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

// The terminal to open a folder in.
//
// `$TERMINAL` first, because a person who set it has already answered this
// question, and then the emulators an Omarchy machine actually has, newest
// desktop first. Every one of them starts its shell in the directory it was
// launched from, so the folder is passed as the working directory and not as a
// flag -- the flag for it is spelled differently by all of them.
QString findTerminal()
{
    const QByteArray named = qgetenv("TERMINAL");
    if (!named.isEmpty()) {
        const QString found = findExecutable(QString::fromLocal8Bit(named));
        if (!found.isEmpty())
            return found;
    }
    static const char *const known[] = {"alacritty", "ghostty", "kitty", "foot",
                                        "wezterm", "x-terminal-emulator",
                                        "gnome-terminal", "konsole", "xterm"};
    for (const char *name : known) {
        const QString found = findExecutable(QString::fromLatin1(name));
        if (!found.isEmpty())
            return found;
    }
    return {};
}

// `$VISUAL` before `$EDITOR`, which is the order every program that reads them
// uses: the first names the editor for a window, the second the one for a pipe.
QString findEditorCommand()
{
    for (const char *name : {"VISUAL", "EDITOR"}) {
        const QByteArray value = qgetenv(name);
        if (value.isEmpty())
            continue;
        const QString found = findExecutable(QString::fromLocal8Bit(value));
        if (!found.isEmpty())
            return found;
    }
    return {};
}

// Where the desktop records which application answers for which type, in the
// order the specification gives: the user's own choices before the system's.
QStringList associationFiles()
{
    QStringList files;
    const QByteArray config = qgetenv("XDG_CONFIG_HOME");
    files.append(QDir(config.isEmpty() ? QDir::homePath() + QStringLiteral("/.config")
                                       : QString::fromLocal8Bit(config))
                     .filePath(QStringLiteral("mimeapps.list")));
    const QByteArray configDirs = qgetenv("XDG_CONFIG_DIRS");
    const QString rest = configDirs.isEmpty() ? QStringLiteral("/etc/xdg")
                                              : QString::fromLocal8Bit(configDirs);
    for (const QString &dir : rest.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        files.append(QDir(dir).filePath(QStringLiteral("mimeapps.list")));
    const QStringList apps = applicationDirs();
    for (const QString &dir : apps)
        files.append(QDir(dir).filePath(QStringLiteral("mimeapps.list")));
    return files;
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

// The types a file answers to: its own, and everything its own is a kind of.
// A `text/markdown` file is a `text/plain` file too, and an editor registered
// only for the second still opens the first.
// Both answers, and everything above them.
//
// The desktop's, through `xdg-mime`, because that is the one the associations
// were written against -- and Qt's own, because the two disagree often enough
// to matter and an application registered under either name still opens the
// file. A machine with no `xdg-mime` on it has only the second, which is the
// other reason not to make the first the only one asked.
static QStringList typesFor(const QString &path, const QString &mime)
{
    static QMimeDatabase database;
    QStringList types;
    const auto note = [&types](const QMimeType &type) {
        if (!type.isValid())
            return;
        if (!types.contains(type.name()))
            types.append(type.name());
        for (const QString &ancestor : type.allAncestors()) {
            if (!types.contains(ancestor))
                types.append(ancestor);
        }
    };
    if (!mime.isEmpty()) {
        types.append(mime);
        note(database.mimeTypeForName(mime));
    }
    note(database.mimeTypeForFile(path, QMimeDatabase::MatchDefault));
    return types;
}

QVector<OpenApp> applicationsFor(const QString &path)
{
    const QString resolved = canonicalPath(path, nullptr);
    if (resolved.isEmpty())
        return {};

    const QString mime = queryMime(resolved);
    const QStringList types = typesFor(resolved, mime);
    if (types.isEmpty())
        return {};

    // The default first, because it is the one the plain `Open` above would
    // take, and a list that buries it is a list you have to read to find out
    // what would have happened anyway.
    QStringList wanted;
    const QString fallback = queryDefaultDesktop(mime);
    if (!fallback.isEmpty())
        wanted.append(fallback);

    QSet<QString> removed;
    for (const QString &file : associationFiles()) {
        const QMap<QString, QString> defaults =
            desktopSection(file, QStringLiteral("Default Applications"));
        const QMap<QString, QString> added =
            desktopSection(file, QStringLiteral("Added Associations"));
        const QMap<QString, QString> takenAway =
            desktopSection(file, QStringLiteral("Removed Associations"));
        for (const QString &type : types) {
            // A removal is only about the file it was written in and the ones
            // below it, so it is collected as the search walks down and never
            // applied to a choice made above it.
            for (const QString &id : desktopIds(takenAway.value(type)))
                removed.insert(id);
            for (const QString &id : desktopIds(defaults.value(type))) {
                if (!removed.contains(id) && !wanted.contains(id))
                    wanted.append(id);
            }
            for (const QString &id : desktopIds(added.value(type))) {
                if (!removed.contains(id) && !wanted.contains(id))
                    wanted.append(id);
            }
        }
    }

    for (const QString &dir : applicationDirs()) {
        const QMap<QString, QString> cache =
            desktopSection(QDir(dir).filePath(QStringLiteral("mimeinfo.cache")),
                           QStringLiteral("MIME Cache"));
        for (const QString &type : types) {
            for (const QString &id : desktopIds(cache.value(type))) {
                if (!removed.contains(id) && !wanted.contains(id))
                    wanted.append(id);
            }
        }
    }

    QVector<OpenApp> apps;
    for (const QString &id : std::as_const(wanted)) {
        const QString file = findDesktopFile(id);
        if (file.isEmpty())
            continue;
        const QMap<QString, QString> entry =
            desktopSection(file, QStringLiteral("Desktop Entry"));
        // What the desktop menu leaves out, this leaves out too: an entry
        // marked NoDisplay is machinery, and one marked Hidden is deleted.
        if (entry.value(QStringLiteral("NoDisplay")).toLower() == QLatin1String("true")
            || entry.value(QStringLiteral("Hidden")).toLower() == QLatin1String("true"))
            continue;
        const QString name = entry.value(QStringLiteral("Name"));
        apps.append({file, name.isEmpty() ? QFileInfo(file).completeBaseName() : name});
    }
    return apps;
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

// A shell where the listing is, which is the one thing a file manager is asked
// for that has nothing to do with any file in it.
bool desktopTerminal(const QString &directory, QString *error)
{
    const QString terminal = findTerminal();
    if (terminal.isEmpty()) {
        if (error)
            *error = QStringLiteral("terminal: none found (set $TERMINAL)");
        return false;
    }
    if (!QProcess::startDetached(terminal, {}, directory)) {
        if (error)
            *error = QStringLiteral("terminal failed: %1 in %2").arg(terminal, directory);
        return false;
    }
    return true;
}

// The editor, asked for by name rather than reached by mime.
//
// `omarchy-launch-editor` first, because it is the one thing here that knows
// how to put a terminal editor in a window -- the same branch `planOpen` takes
// for a text file. Without it, `$VISUAL` or `$EDITOR` inside a terminal, which
// is `-e` on every emulator `findTerminal` names but one. Nothing else: falling
// back to the desktop's default would make this entry silently mean `Open`, and
// two entries that do the same thing are worse than one that says it cannot.
bool desktopEdit(const QString &path, QString *error)
{
    const QString omarchyEditor = findExecutable(QStringLiteral("omarchy-launch-editor"));
    if (!omarchyEditor.isEmpty()) {
        if (!QProcess::startDetached(omarchyEditor, {path})) {
            if (error)
                *error = QStringLiteral("edit failed: omarchy-launch-editor %1").arg(path);
            return false;
        }
        return true;
    }

    const QString editor = findEditorCommand();
    const QString terminal = findTerminal();
    if (editor.isEmpty() || terminal.isEmpty()) {
        if (error)
            *error = QStringLiteral("edit: no editor (set $EDITOR, or install omarchy-launch-editor)");
        return false;
    }
    if (!QProcess::startDetached(terminal, {QStringLiteral("-e"), editor, path},
                                 QFileInfo(path).absolutePath())) {
        if (error)
            *error = QStringLiteral("edit failed: %1 -e %2 %3").arg(terminal, editor, path);
        return false;
    }
    return true;
}

// The entry, launched as the entry says.
//
// `gio launch` and not the `Exec=` line read by hand: that line carries field
// codes -- `%f`, `%U`, `%c`, `%i` -- and quoting rules of its own, and a
// launcher that expands them itself is a launcher that gets one of them wrong
// for somebody's file name. glib already does it, and `trash` already needs it.
bool desktopOpenWith(const QString &path, const QString &desktopFile, QString *error)
{
    const QString gio = findExecutable(QStringLiteral("gio"));
    if (gio.isEmpty()) {
        if (error)
            *error = QStringLiteral("open with: gio not found (it comes with glib2)");
        return false;
    }
    if (!QProcess::startDetached(gio, {QStringLiteral("launch"), desktopFile, path})) {
        if (error)
            *error = QStringLiteral("open with failed: gio launch %1 %2").arg(desktopFile, path);
        return false;
    }
    return true;
}

// Detached, the way every other door here is: a script that opens an editor
// and waits for it is not a script the window should wait for.
bool desktopScript(const OpenRequest &request, QString *error)
{
    if (!QProcess::startDetached(request.application, request.arguments, request.path)) {
        if (error)
            *error = QStringLiteral("script failed to start: %1").arg(request.application);
        return false;
    }
    return true;
}

bool desktopLauncher(const OpenRequest &request, QString *error)
{
    switch (request.kind) {
    case OpenRequest::Share:
        return desktopShare(request.path, error);
    case OpenRequest::Terminal:
        return desktopTerminal(request.path, error);
    case OpenRequest::Editor:
        return desktopEdit(request.path, error);
    case OpenRequest::OpenWith:
        return desktopOpenWith(request.path, request.application, error);
    case OpenRequest::Script:
        return desktopScript(request, error);
    case OpenRequest::Open:
        break;
    }
    return desktopOpen(request.path, error);
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
    return installedLauncher()(OpenRequest{OpenRequest::Open, path, QString(), {}}, error);
}

bool shareLocalPath(const QString &path, QString *error)
{
    return installedLauncher()(OpenRequest{OpenRequest::Share, path, QString(), {}}, error);
}

bool canOpenTerminal()
{
    return !findTerminal().isEmpty();
}

bool canOpenInEditor()
{
    return !findExecutable(QStringLiteral("omarchy-launch-editor")).isEmpty()
        || (!findEditorCommand().isEmpty() && !findTerminal().isEmpty());
}

bool openTerminalIn(const QString &directory, QString *error)
{
    return installedLauncher()(OpenRequest{OpenRequest::Terminal, directory, QString(), {}}, error);
}

bool openInEditor(const QString &path, QString *error)
{
    return installedLauncher()(OpenRequest{OpenRequest::Editor, path, QString(), {}}, error);
}

// Beside the views, and resolved the same way, so the two things a person
// writes for this program live in one place.
QString scriptsDirectory()
{
    QString configHome = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    if (configHome.isEmpty())
        configHome = QDir::homePath() + QStringLiteral("/.config");
    return QDir(configHome).filePath(QStringLiteral("omafiles/scripts"));
}

QVector<UserScript> userScripts()
{
    QVector<UserScript> scripts;
    const QDir dir(scriptsDirectory());
    // Executable and a file: a folder is not a script, and a text somebody
    // forgot to chmod is a script that would only ever fail.
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Executable,
                                                    QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries)
        scripts.append({entry.fileName(), entry.absoluteFilePath()});
    return scripts;
}

bool runUserScript(const QString &script, const QStringList &paths, const QString &directory,
                   QString *error)
{
    if (!QFileInfo(script).isExecutable()) {
        if (error)
            *error = QStringLiteral("%1 is not executable").arg(script);
        return false;
    }
    return installedLauncher()(OpenRequest{OpenRequest::Script, directory, script, paths},
                               error);
}

bool canOpenWithApp()
{
    return !findExecutable(QStringLiteral("gio")).isEmpty();
}

bool openWithApp(const QString &path, const QString &desktopFile, QString *error)
{
    return installedLauncher()(OpenRequest{OpenRequest::OpenWith, path, desktopFile, {}}, error);
}

} // namespace omafiles
