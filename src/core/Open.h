#pragma once

#include "Paths.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace omafiles {

struct OpenPlan {
    QString program;
    QStringList arguments;
    QString mime;
    QString desktopId;

    QJsonObject toJson() const;
};

/// The program the desktop would use for `path`, and its arguments.
///
/// `state` is what the system said about the path, for the caller that has to
/// tell a path that is not there from one it may not look at -- and from a
/// desktop with nothing to open it with, which is the failure that leaves the
/// path Present.
bool planOpen(const QString &path, OpenPlan *plan, QString *error,
              PathState *state = nullptr);

/// One application the desktop would let a file be opened with.
struct OpenApp {
    /// The absolute path of the `.desktop` entry, which is what launches it and
    /// what names it: two applications can share a `Name`, and the entry cannot
    /// be shared.
    QString desktopFile;
    QString name;
};

/// One request to hand a path to somebody else's program.
///
/// `path` is the file for every kind but `Terminal`, where it is the directory
/// the shell is to start in. `application` is the `.desktop` entry `OpenWith`
/// picked, and empty for every other kind.
struct OpenRequest {
    enum Kind { Open, Share, Terminal, Editor, OpenWith };
    Kind kind = Open;
    QString path;
    QString application;
};

/// The one door out of omafiles.
///
/// `openLocalPath`, `shareLocalPath`, `openTerminalIn` and `openInEditor` are
/// the only calls that start a program of the desktop's choosing, and all of
/// them go through here. It is a seam so that a caller can be handed the request
/// as a value instead of a running browser -- which is what the studio test
/// suite does, before `main`, for all of its cases at once.
///
/// One function and not four, so there is no way to redirect `open` and leave
/// the other three pointed at the desktop. A test that steers around the call by
/// hand only protects the cases that remember to.
using Launcher = std::function<bool(const OpenRequest &request, QString *error)>;

/// Installs `launcher` and returns the one it replaced, so a caller can put back
/// what it found. An empty `launcher` restores the desktop.
Launcher setLauncher(Launcher launcher);

bool openLocalPath(const QString &path, QString *error);
bool canShareLocalSend();
bool shareLocalPath(const QString &path, QString *error);

/// Whether there is a terminal on this machine to open a folder in, and an
/// editor to hand a file to. Asked before the entry is drawn rather than after
/// it is chosen: a menu item that can only ever fail is worse than no item.
bool canOpenTerminal();
bool canOpenInEditor();
bool openTerminalIn(const QString &directory, QString *error);
bool openInEditor(const QString &path, QString *error);

/// Every application registered for what `path` is, the default one first.
///
/// The desktop's own answer, read where the desktop keeps it: the association
/// lists in `mimeapps.list` and the `mimeinfo.cache` each applications
/// directory carries. Not `gio mime`, whose output is prose with translated
/// labels in it -- a list parsed out of that is a list that empties itself on a
/// machine running in another language.
QVector<OpenApp> applicationsFor(const QString &path);

/// Whether an application can be started by its entry at all, which is `gio`
/// being installed -- the same glib2 `trash` already needs.
bool canOpenWithApp();
bool openWithApp(const QString &path, const QString &desktopFile, QString *error);

QString formatOpenPlan(const OpenPlan &plan);

} // namespace omafiles

