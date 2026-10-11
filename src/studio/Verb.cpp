#include "Verb.h"

#include "Open.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include <utility>

namespace omafiles {

namespace {

QString firstLine(const QString &text)
{
    const QString trimmed = text.trimmed();
    const int cut = trimmed.indexOf(QLatin1Char('\n'));
    return cut < 0 ? trimmed : trimmed.left(cut);
}

// The environment is not a vote.
//
// The three globals the command line reads from the environment are answers to
// a question typed at a prompt, and nobody typed anything here. An
// `OMAFILES_DRY_RUN=1` exported in a shell months ago would turn every entry in
// this menu into a no-op that reported success; `OMAFILES_JSON` would turn the
// receipt this reads into a document it does not parse; `OMAFILES_YES` is the
// window's to give per verb, not the environment's to give for all of them.
QProcessEnvironment childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("OMAFILES_JSON"));
    env.remove(QStringLiteral("OMAFILES_DRY_RUN"));
    env.remove(QStringLiteral("OMAFILES_YES"));
    return env;
}

void processRunner(const QStringList &arguments, VerbReceipt done)
{
    const QString program = verbProgram();
    if (program.isEmpty()) {
        done(false, QString(),
             QStringLiteral("omafiles: the command line is not installed beside the window"));
        return;
    }

    auto *proc = new QProcess;
    proc->setProcessEnvironment(childEnvironment());
    QObject::connect(proc, &QProcess::finished, proc,
                     [proc, done, arguments](int code, QProcess::ExitStatus status) {
        const QString output = QString::fromUtf8(proc->readAllStandardOutput());
        const QString diagnostics = QString::fromUtf8(proc->readAllStandardError());
        proc->deleteLater();
        if (status == QProcess::NormalExit && code == 0) {
            done(true, output, QString());
            return;
        }
        // The command line says why on stderr and says it well, so that line is
        // the message rather than a sentence written here about an exit code.
        // Only where it said nothing at all does the status stand in for it.
        const QString said = firstLine(diagnostics);
        done(false, output,
             said.isEmpty() ? QStringLiteral("omafiles %1: exit %2")
                                  .arg(arguments.value(0)).arg(code)
                            : said);
    });
    // Only the one failure `finished` never reports. A crash raises both
    // signals, and a receipt delivered twice for one run would have the status
    // bar arguing with itself.
    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [proc, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const QString said = proc->errorString();
        proc->deleteLater();
        done(false, QString(), QStringLiteral("omafiles: %1").arg(said));
    });
    proc->start(program, arguments);
}

// A function-local static, for the reason `Open.cpp` gives about its own: the
// studio test binary installs its recorder from a static initialiser, before
// `main`, and a namespace-scope runner might not have been constructed yet.
VerbRunner &installedRunner()
{
    static VerbRunner runner = processRunner;
    return runner;
}

} // namespace

VerbRunner setVerbRunner(VerbRunner runner)
{
    VerbRunner previous = installedRunner();
    installedRunner() = runner ? std::move(runner) : VerbRunner(processRunner);
    return previous;
}

// Beside the window first, and only then on the PATH.
//
// The two binaries are built together, installed together and versioned
// together, so a studio run out of a checkout has to speak to that checkout's
// command line -- not to a copy somebody installed a year ago, whose index
// format it may no longer share.
QString verbProgram()
{
    const QString beside =
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("omafiles"));
    if (QFileInfo(beside).isExecutable())
        return beside;
    return findExecutable(QStringLiteral("omafiles"));
}

void runVerb(const QStringList &arguments, VerbReceipt done)
{
    installedRunner()(arguments, std::move(done));
}

} // namespace omafiles
