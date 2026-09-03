#pragma once

#include "Paths.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

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

/// One request to hand a path to somebody else's program.
struct OpenRequest {
    enum Kind { Open, Share };
    Kind kind = Open;
    QString path;
};

/// The one door out of omafiles.
///
/// `openLocalPath` and `shareLocalPath` are the only two calls that start a
/// program of the desktop's choosing, and both of them go through here. It is a
/// seam so that a caller can be handed the request as a value instead of a
/// running browser -- which is what the studio test suite does, before `main`,
/// for all of its cases at once.
///
/// One function and not two, so there is no way to redirect `open` and leave
/// `share` pointed at the desktop. A test that steers around the call by hand
/// only protects the cases that remember to.
using Launcher = std::function<bool(const OpenRequest &request, QString *error)>;

/// Installs `launcher` and returns the one it replaced, so a caller can put back
/// what it found. An empty `launcher` restores the desktop.
Launcher setLauncher(Launcher launcher);

bool openLocalPath(const QString &path, QString *error);
bool canShareLocalSend();
bool shareLocalPath(const QString &path, QString *error);
QString formatOpenPlan(const OpenPlan &plan);

} // namespace omafiles

