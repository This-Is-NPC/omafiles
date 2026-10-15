#include "ArchiveJob.h"
#include "../core/Paths.h"
#include "../core/Open.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <unistd.h>

namespace omafiles {
namespace {
struct Stage {
    QString path;
    ~Stage() {
        const QString directory = path;
        if (!directory.isEmpty()) {
            auto cleanup = QtConcurrent::run([directory] {
                QString leaf;
                const int parent = openDirectoryNoFollow(typedDirectory(directory, &leaf));
                if (parent >= 0) {
                    removeTreeAt(parent, QFile::encodeName(leaf));
                    ::close(parent);
                }
            });
            Q_UNUSED(cleanup)
        }
    }
};
}
struct ArchiveJob::Private {
    std::unique_ptr<QProcess> process = std::make_unique<QProcess>();
    std::shared_ptr<Stage> stage;
    QStringList paths;
    QByteArray input;
    QString destination, label, progress, error;
    qint64 processed = 0;
    bool busy = false, canceled = false, prepared = false;
};
ArchiveJob::ArchiveJob(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {
    auto receive = [this] {
        d->input += d->process->readAllStandardOutput();
        for (;;) {
            const auto newline = d->input.indexOf('\n');
            if (newline > 1024 * 1024 || (newline < 0 && d->input.size() > 1024 * 1024)) {
                d->error = "Archive helper returned too much data.";
                d->process->kill();
                return;
            }
            if (newline < 0) break;
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(d->input.left(newline), &parse);
            d->input.remove(0, newline + 1);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) {
                d->error = "Archive helper returned invalid data.";
                d->process->kill();
                return;
            }
            const auto message = document.object();
            if (message.contains("error")) d->error = message.value("error").toString();
            if (message.contains("entry")) {
                const QString name = message.value("entry").toString();
                if (name.isEmpty() || name == "." || name == ".." || name.contains('/')) {
                    d->error = "Archive helper returned an unsafe output path.";
                    d->process->kill();
                    return;
                }
                d->paths.append(QDir(d->stage->path).filePath(name));
            }
            if (message.value("ready").toBool()) d->prepared = true;
            if (message.contains("processed") && !d->canceled) {
                d->processed = message.value("processed").toInteger();
                d->progress = QString("Preparing archive: %1 entries, %2 bytes")
                    .arg(message.value("entries").toInteger()).arg(d->processed);
                emit changed();
            }
        }
    };
    connect(d->process.get(), &QProcess::readyReadStandardOutput, this, receive);
    connect(d->process.get(), &QProcess::readyReadStandardError, this, [this] { d->process->readAllStandardError(); });
    connect(d->process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(false, "Cannot start archive helper: " + d->process->errorString());
    });
    connect(d->process.get(), &QProcess::finished, this, [this, receive](int code, QProcess::ExitStatus status) {
        receive();
        if (d->canceled) complete(false, "Archive operation canceled.");
        else if (status != QProcess::NormalExit || code != 0 || !d->prepared || !d->input.isEmpty() || !d->error.isEmpty())
            complete(false, d->error.isEmpty() ? "Archive helper stopped before preparing the files." : d->error);
        else {
            d->progress = "Publishing archive files";
            emit changed();
            emit ready(d->paths, d->destination, d->label);
        }
    });
}
ArchiveJob::~ArchiveJob() {
    QProcess *process = d->process.release();
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) delete process;
    else {
        const auto stage = d->stage;
        connect(process, &QProcess::finished, process, [process, stage] { process->deleteLater(); });
        process->kill();
    }
}
bool ArchiveJob::busy() const { return d->busy; }
bool ArchiveJob::canceled() const { return d->canceled; }
QString ArchiveJob::progress() const { return d->progress; }
qint64 ArchiveJob::processed() const { return d->processed; }
void ArchiveJob::compress(const QStringList &paths, const QString &destination, const QString &format) {
    start(paths, destination, format, false);
}
void ArchiveJob::extract(const QString &path, const QString &destination) {
    start({path}, destination, {}, true);
}
void ArchiveJob::start(const QStringList &paths, const QString &destination, const QString &format, bool extracting) {
    if (d->busy) return;
    d->busy = true;
    d->canceled = false;
    d->prepared = false;
    d->processed = 0;
    d->paths.clear(); d->input.clear(); d->error.clear();
    d->progress = "Preparing archive";
    emit changed();
    const QString directory = extracting ? destination : typedDirectory(anchoredPath(destination), nullptr);
    const int descriptor = openDirectoryNoFollow(directory);
    if (descriptor < 0) { complete(false, "Archive destination must be an accessible directory without symbolic links."); return; }
    ::close(descriptor);
    QTemporaryDir temporary(QDir(directory).filePath(".omafiles-archive-XXXXXX"));
    if (!temporary.isValid()) { complete(false, "Cannot create private archive staging directory in the destination."); return; }
    d->stage = std::make_shared<Stage>();
    d->stage->path = temporary.path();
    temporary.setAutoRemove(false);
    d->destination = directory;
    d->label = extracting ? "Extract archive" : "Compress files";
    QString program = QDir(QCoreApplication::applicationDirPath()).filePath("omafiles-archive");
    if (!QFileInfo(program).isExecutable()) program = findExecutable("omafiles-archive");
    if (program.isEmpty()) { complete(false, "Archive helper is unavailable. Install omafiles with mise run install."); return; }
    QJsonArray selected;
    for (const auto &path : paths) selected.append(anchoredPath(path));
    const QByteArray request = QJsonDocument(QJsonObject{{"paths", selected}, {"destination", anchoredPath(destination)},
        {"format", format}, {"extracting", extracting}, {"stage", d->stage->path}}).toJson(QJsonDocument::Compact);
    if (request.size() > 1024 * 1024) { complete(false, "Archive selection is too large."); return; }
    d->process->start(program, {});
    d->process->write(request);
    d->process->closeWriteChannel();
}
void ArchiveJob::cancel() {
    if (!d->busy) return;
    d->canceled = true;
    d->progress = "Canceling archive operation";
    emit changed();
    if (d->process->state() != QProcess::NotRunning) d->process->kill();
}
void ArchiveJob::updateProgress(const QString &message) {
    if (!d->busy || d->canceled) return;
    d->progress = message;
    emit changed();
}
void ArchiveJob::complete(bool ok, const QString &message) {
    if (!d->busy || d->process->state() != QProcess::NotRunning) return;
    d->stage.reset();
    d->busy = false;
    d->progress = message;
    emit changed();
    emit finished(ok, message);
}
}
