#pragma once

#include <QString>
#include <QStringList>

#include <functional>

namespace omafiles {

/// A CLI result: success, stdout, and stderr notes or a refusal.
using VerbReceipt = std::function<void(bool ok, const QString &output, const QString &diagnostics)>;

/// Keep successful CLI notes beside the window's summary.
QString verbNotice(bool ok, const QString &success, const QString &diagnostics);

/// One request to run a verb of the command line.
///
/// The window does not reimplement `tag add`, `trash`, `mv` or `reindex`. It
/// runs the binary that already has them, because the refusals are the valuable
/// part: `mv` will not clobber a destination, `trash` will not do half of a set
/// it cannot do all of, and a second copy of those rules in QML is a second copy
/// that will drift. One implementation, two front ends.
using VerbRunner = std::function<void(const QStringList &arguments, VerbReceipt done)>;

/// Installs `runner` and returns the one it replaced. An empty `runner` puts
/// the child process back. The studio test suite installs a recorder here for
/// the same reason it installs one over `Launcher`: nothing in that binary is
/// allowed to touch the machine it runs on.
VerbRunner setVerbRunner(VerbRunner runner);

/// The `omafiles` binary this window speaks to, or empty where there is none.
QString verbProgram();

/// The journaled file or tag verb, or empty for other commands.
QString journalVerb(const QStringList &arguments);

/// Runs `omafiles <arguments>`; journaled commands request JSON receipts.
///
/// Nothing blocks. `reindex` over a home directory is minutes of walking, and a
/// window that stops answering the keyboard for it is a window that looks
/// crashed -- so the receipt arrives on the event loop rather than being waited
/// for at the call.
void runVerb(const QStringList &arguments, VerbReceipt done);

} // namespace omafiles
