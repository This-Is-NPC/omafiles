#include "DefaultManager.h"
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include "IndexModel.h"

#include "Open.h"
#include "Paths.h"
#include "Roots.h"
#include "Verb.h"
#include "UndoJournal.h"
#include "Tag.h"
#include "Xattr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>

#include <cerrno>
#include <functional>
#include <memory>

#include <sys/stat.h>
#include <unistd.h>

namespace omafiles {

class TagListModel : public QAbstractListModel
{
public:
    enum Roles { TagRole = Qt::UserRole + 1, CountRole };
    explicit TagListModel(IndexModel *owner)
        : QAbstractListModel(owner)
    {
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        if (parent.isValid())
            return 0;
        return m_tags.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_tags.size())
            return {};
        const auto &row = m_tags.at(index.row());
        if (role == TagRole || role == Qt::DisplayRole)
            return row.tag;
        if (role == CountRole)
            return row.count;
        return {};
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{TagRole, "tag"}, {CountRole, "count"}};
    }

    void rebuild(const QMap<QString, int> &counts)
    {
        beginResetModel();
        m_tags.clear();
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            m_tags.push_back({it.key(), it.value()});
        endResetModel();
    }

private:
    struct Row {
        QString tag;
        int count = 0;
    };
    QVector<Row> m_tags;
};

IndexModel::IndexModel(QObject *parent)
    : QObject(parent)
    , m_rootsPath(defaultRootsPath())
    , m_store(defaultIndexPath())
{
    m_archive = new ArchiveJob(this);
    connect(m_archive, &ArchiveJob::changed, this, &IndexModel::undoChanged);
    connect(m_archive, &ArchiveJob::ready, this, &IndexModel::publishArchive);
    connect(m_archive, &ArchiveJob::finished, this, [this](bool, const QString &message) {
        reload();
        if (m_archiveContext == m_noticeContext) setNotice(message);
    });
    m_undo = new UndoJournal(this);
    connect(m_undo, &UndoJournal::conflictRequested, this, &IndexModel::conflictRequested);
    connect(m_undo, &UndoJournal::changed, this, &IndexModel::undoChanged);
    m_tags = new TagListModel(this);
    // Prepare owned state before watching it, so initialization does not trigger a reload.
    for (const QString &path : {m_store.indexPath(), m_rootsPath})
        makePath(typedDirectory(path, nullptr), nullptr);
    reload();

    m_settle.setSingleShot(true);
    m_settle.setInterval(60);
    connect(&m_settle, &QTimer::timeout, this, &IndexModel::reload);
    const auto later = [this] { m_settle.start(); emit loadingChanged(); };
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, later);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, later);
}

int IndexModel::tagCount() const
{
    return m_tags->rowCount();
}

QAbstractListModel *IndexModel::tagsModel() const
{
    return m_tags;
}

QStringList IndexModel::tagsForPath(const QString &path) const
{
    // A row counts where `rowState` says it is the file, as `ls` reads one.
    const Record row = m_store.get(path);
    if (row.path.isEmpty() || rowState(path, nullptr) != PathState::Present)
        return {};
    return row.tags;
}

QStringList IndexModel::tagChoices(const QStringList &paths) const
{
    QStringList tags;
    for (const QString &path : paths)
        tags += m_store.get(path).tags;
    return uniqueSorted(tags);
}

struct IndexModel::Presence {
    int present = 0;
    int missing = 0;
    int unknown = 0;
    int unreachable = 0;
    QString error;
    QMap<QString, int> tags;
};

IndexModel::~IndexModel()
{
    if (m_presenceCancellation)
        m_presenceCancellation->store(true, std::memory_order_relaxed);
}

IndexModel::Presence IndexModel::countPresence(const QMap<QString, Record> &records,
                                             const std::shared_ptr<std::atomic_bool> &cancelled)
{
    Presence result;
    QStringList storageWarnings;
    for (const Root &root : roots()) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return result;
        QString detail;
        const PathState state = rootState(root, &detail);
        if (state == PathState::Unreachable)
            storageWarnings.append(unreachableReason(root.path, root));
        else if (state == PathState::Unknown)
            storageWarnings.append(QStringLiteral("root %1 cannot be checked: %2").arg(root.id, detail));
    }
    QString declined;
    for (const Record &record : records) {
        if (cancelled && cancelled->load(std::memory_order_relaxed))
            return result;
        QString detail;
        switch (rowState(record.path, &detail)) {
        case PathState::Present:
            ++result.present;
            for (const QString &tag : record.tags)
                ++result.tags[tag];
            break;
        case PathState::Missing:
            ++result.missing;
            break;
        case PathState::Unknown:
            ++result.unknown;
            if (declined.isEmpty())
                declined = detail;
            break;
        case PathState::Unreachable:
            ++result.unreachable;
            break;
        }
    }
    if (result.unreachable > 0)
        storageWarnings.prepend(QStringLiteral("%1 index row(s) are on a root that is not here")
                                    .arg(result.unreachable));
    if (result.unknown > 0)
        storageWarnings.append(QStringLiteral("%1 index row(s) not counted; %2").arg(result.unknown).arg(declined));
    if (result.missing > 0 && result.present == 0)
        storageWarnings.append(QStringLiteral("%1 index row(s), and none of their files are there; "
                                              "omafiles prune drops them").arg(result.missing));
    result.error = storageWarnings.join(QStringLiteral("; "));
    return result;
}

void IndexModel::publishPresence(const Presence &presence, bool clearReceipt)
{
    const QString error = m_loadError.isEmpty() ? presence.error : m_loadError;
    if (clearReceipt && (m_present != presence.present || m_missing != presence.missing
        || m_unknown != presence.unknown || m_error != error))
        clearNotice();
    m_present = presence.present;
    m_missing = presence.missing;
    m_unknown = presence.unknown;
    m_error = error;
    m_tags->rebuild(presence.tags);
    emit changed();
}

void IndexModel::reload()
{
    m_settle.stop();
    loadSources();
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_presenceCancellation = cancelled;
    const auto records = m_store.recordSnapshot();
    const auto context = m_noticeContext;
    auto *watcher = new QFutureWatcher<Presence>(this);
    connect(watcher, &QFutureWatcher<Presence>::finished, this, [this, watcher, cancelled, context] {
        watcher->deleteLater();
        if (!cancelled->load(std::memory_order_relaxed)) {
            m_presenceCancellation.reset();
            publishPresence(watcher->result(), context == m_noticeContext);
            emit loadingChanged();
        }
    });
    watcher->setFuture(QtConcurrent::run([records, cancelled] { return countPresence(records, cancelled); }));
    emit loadingChanged();
}

void IndexModel::loadSources()
{
    if (m_presenceCancellation)
        m_presenceCancellation->store(true, std::memory_order_relaxed);
    const auto previous = m_store.recordSnapshot();
    const auto previousRoots = roots();
    m_loadError.clear();
    if (loadRoots(m_rootsPath, &m_loadError))
        m_store.load(&m_loadError);
    if (previous != m_store.recordSnapshot() || previousRoots != roots()) clearNotice();
    if (previousRoots != roots()) emit storageChanged();
    watchSources();
}

void IndexModel::watchSources()
{
    for (const QString &path : {m_store.indexPath(), m_rootsPath}) {
        if (QFileInfo::exists(path) && !m_watcher.files().contains(path))
            m_watcher.addPath(path);
        QString directory = typedDirectory(path, nullptr);
        while (!QFileInfo::exists(directory) && directory != typedDirectory(directory, nullptr))
            directory = typedDirectory(directory, nullptr);
        if (!m_watcher.directories().contains(directory))
            m_watcher.addPath(directory);
    }
}

void IndexModel::refreshStorage()
{
    ++m_noticeContext;
    clearNotice();
    reload();
    emit storageChanged();
}

void IndexModel::openPath(const QString &path)
{
    changeNoticeContext(m_noticeDirectory);
    QString error;
    if (!openLocalPath(path, &error)) {
        setNotice(error);
    }
}

bool IndexModel::canShare() const
{
    return canShareLocalSend();
}

void IndexModel::sharePath(const QString &path)
{
    changeNoticeContext(m_noticeDirectory);
    QString error;
    if (!shareLocalPath(path, &error)) {
        setNotice(error);
    }
}

void IndexModel::makeDefaultFileManager()
{
    if (m_settingDefault)
        return;
    m_settingDefault = true;
    setNotice(QStringLiteral("Setting the default file manager…"));
    auto *watcher = new QFutureWatcher<DefaultManagerResult>(this);
    connect(watcher, &QFutureWatcher<DefaultManagerResult>::finished, this, [this, watcher, receive = noticeReceiver()] {
        const auto result = watcher->result();
        m_settingDefault = false;
        watcher->deleteLater();
        receive(result.ok ? QStringLiteral("Default file manager selected")
                            : QStringLiteral("Default file manager selection incomplete"));
        emit defaultManagerFinished(result.ok, result.text);
    });
    watcher->setFuture(QtConcurrent::run(omafiles::makeDefaultFileManager,
                                        QCoreApplication::applicationFilePath()));
}

void IndexModel::openTerminalIn(const QString &directory)
{
    QString error;
    if (!omafiles::openTerminalIn(directory, &error))
        setNotice(error);
}

void IndexModel::editPath(const QString &path)
{
    QString error;
    if (!openInEditor(path, &error))
        setNotice(error);
}

QString IndexModel::openCommandFor(const QString &path) const
{
    OpenPlan plan;
    QString error;
    if (!planOpen(path, &plan, &error))
        return QString();
    return formatOpenPlan(plan);
}

QVariantList IndexModel::applicationsFor(const QString &path) const
{
    QVariantList out;
    const QVector<OpenApp> apps = omafiles::applicationsFor(path);
    for (const OpenApp &app : apps) {
        out.append(QVariantMap{{QStringLiteral("file"), app.desktopFile},
                               {QStringLiteral("name"), app.name}});
    }
    return out;
}

void IndexModel::openWith(const QString &path, const QString &desktopFile)
{
    QString error;
    if (!openWithApp(path, desktopFile, &error))
        setNotice(error);
}

void IndexModel::noApplicationFor(const QString &path)
{
    setNotice(QStringLiteral("no application registered for %1")
                  .arg(QFileInfo(path).fileName()));
}

bool IndexModel::canOpenWith() const
{
    return canOpenWithApp();
}

bool IndexModel::canOpenTerminal() const
{
    return omafiles::canOpenTerminal();
}

bool IndexModel::canEdit() const
{
    return canOpenInEditor();
}

bool IndexModel::hasScripts() const
{
    return !userScripts().isEmpty();
}

QVariantList IndexModel::scripts() const
{
    QVariantList out;
    for (const UserScript &script : userScripts()) {
        out.append(QVariantMap{{QStringLiteral("name"), script.name},
                               {QStringLiteral("path"), script.path}});
    }
    return out;
}


namespace {
QByteArray directoryState(const QString &path)
{
    struct stat state {};
    if (path.isEmpty() || ::stat(QFile::encodeName(path).constData(), &state) != 0)
        return {};
    QByteArray result;
    for (const qint64 value : {qint64(state.st_dev), qint64(state.st_ino),
             qint64(state.st_mtim.tv_sec), qint64(state.st_mtim.tv_nsec),
             qint64(state.st_ctim.tv_sec), qint64(state.st_ctim.tv_nsec)})
        result += QByteArray::number(value) + ':';
    return result;
}
}

void IndexModel::setNotice(const QString &message)
{
    if (!message.isEmpty()) ++m_noticeContext;
    m_noticeDirectoryState = message.isEmpty() ? QByteArray() : directoryState(m_noticeDirectory);
    if (m_notice == message)
        return;
    m_notice = message;
    emit noticeChanged();
}

void IndexModel::changeNoticeContext(const QString &directory)
{
    m_noticeDirectory = directory;
    ++m_noticeContext;
    clearNotice();
}

void IndexModel::expireNoticeForDirectory(const QString &directory)
{
    if (!m_notice.isEmpty() && directory == m_noticeDirectory
        && directoryState(directory) != m_noticeDirectoryState)
        clearNotice();
}

std::function<void(const QString &)> IndexModel::noticeReceiver()
{
    const QPointer<IndexModel> self(this);
    const auto context = ++m_noticeContext;
    return [self, context](const QString &message) {
        if (self && context == self->m_noticeContext) self->setNotice(message);
    };
}

void IndexModel::clearNotice()
{
    setNotice(QString());
}

namespace {

QString countOf(int n, const QString &noun)
{
    return n == 1 ? QStringLiteral("1 %1").arg(noun)
                  : QStringLiteral("%1 %2s").arg(n).arg(noun);
}

} // namespace

void IndexModel::report(const QStringList &arguments, const QString &success, bool tagEdit)
{
    clearNotice();
    if (tagEdit) {
        if (m_tagEditing)
            return;
        m_tagEditing = true;
        emit tagEditingChanged();
    }
    QPointer<IndexModel> self(this);
    const QString verb = journalVerb(arguments);
    const auto runner = !verb.isEmpty()
        ? VerbRunner([this, verb](const QStringList &args, VerbReceipt receipt) {
              const QString operation = verb == QLatin1String("tag") ? QStringLiteral("tag edit")
                  : verb == QLatin1String("cp") ? QStringLiteral("copy")
                  : verb == QLatin1String("mv") ? QStringLiteral("move") : QStringLiteral("trash");
              m_undo->run({args}, operation, std::move(receipt), true);
          }) : VerbRunner(runVerb);
    runner(arguments, [self, success, tagEdit, receive = noticeReceiver()](bool ok, const QString &, const QString &error) {
        if (!self)
            return;
        // Publish the updated index before its operation receipt.
        if (ok || tagEdit)
            self->reload();
        receive(verbNotice(ok, success, error));
        if (tagEdit) {
            self->m_tagEditing = false;
            emit self->tagEditingChanged();
            emit self->tagEditFinished(ok, self->notice());
        }
    });
}

void IndexModel::reportEach(const QVector<QStringList> &runs, const QString &verb)
{
    clearNotice();
    if (runs.isEmpty())
        return;
    QPointer<IndexModel> self(this);
    const QString operation = verb == QLatin1String("copied") ? QStringLiteral("copy") : QStringLiteral("move");
    m_undo->run(runs, operation, [self, verb, asked = runs.size(), receive = noticeReceiver()](bool ok, const QString &output, const QString &error) {
        if (!self)
            return;
        const QString summary = ok ? QStringLiteral("%1 %2").arg(verb, countOf(output.toInt(), QStringLiteral("file")))
            : QStringLiteral("%1 %2 of %3 files").arg(verb).arg(output.toInt()).arg(asked);
        self->reload();
        receive(verbNotice(true, summary, error));
    }, true);
}

bool IndexModel::canUndo() const { return !m_archive->busy() && m_undo->available(); }
bool IndexModel::waitingForVerb() const { return m_undo->waitingForVerb(); }
bool IndexModel::operationBusy() const { return m_archive->busy() || m_undo->busy(); }
QString IndexModel::undoLabel() const { return m_undo->label(); }

void IndexModel::compress(const QStringList &paths, const QString &destination, const QString &format)
{
    if (operationBusy()) { setNotice(QStringLiteral("An operation is still running")); return; }
    clearNotice();
    m_archiveContext = ++m_noticeContext;
    m_archiveTags.clear();
    for (const QString &path : paths) m_archiveTags += tagsForPath(path);
    m_archiveTags = uniqueSorted(m_archiveTags);
    m_archive->compress(paths, destination, format);
}

void IndexModel::extract(const QString &path, const QString &destination)
{
    if (operationBusy()) { setNotice(QStringLiteral("An operation is still running")); return; }
    clearNotice();
    m_archiveContext = ++m_noticeContext;
    m_archiveTags.clear();
    m_archive->extract(path, destination);
}

void IndexModel::cancelArchive()
{
    if (!m_archive->busy()) return;
    m_archive->cancel();
    m_undo->resolveConflict(QStringLiteral("cancel"), false);
}

void IndexModel::publishArchive(const QStringList &paths, const QString &directory, const QString &label)
{
    if (paths.isEmpty()) { m_archive->complete(true, QStringLiteral("Archive contains no files")); return; }
    const QString stage = typedDirectory(paths.first(), nullptr);
    const QStringList tags = m_archiveTags;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, paths, directory, label, stage] {
        const QString error = watcher->result(); watcher->deleteLater();
        if (!error.isEmpty() || m_archive->canceled()) {
            finishArchive(false, error.isEmpty() ? QStringLiteral("Archive operation canceled") : error, stage);
            return;
        }
        QPointer<IndexModel> self(this);
        runVerb({QStringLiteral("reindex"), QStringLiteral("--from-xattr"), QStringLiteral("--"), stage},
                [self, paths, directory, label, stage](bool ok, const QString &, const QString &note) {
            if (!self) return;
            if (!ok || self->m_archive->canceled()) {
                self->finishArchive(false, ok ? QStringLiteral("Archive operation canceled") : note, stage); return;
            }
            self->m_undo->install(paths, directory, label,
                [self, label, stage](bool done, const QString &count, const QString &diagnostics) {
                    if (!self) return;
                    const QString message = label + QStringLiteral(": published %1 entries").arg(count);
                    self->finishArchive(done, verbNotice(true, message, diagnostics), stage);
                }, [self] { return !self || self->m_archive->canceled(); },
                [self](int done, int total) {
                    if (self) self->m_archive->updateProgress(QStringLiteral("Publishing %1 of %2 entries; Cancel stops after the current entry").arg(done).arg(total));
                });
        });
    });
    watcher->setFuture(QtConcurrent::run([paths, directory, tags] {
        const int fd = openDirectoryNoFollow(directory);
        if (fd < 0) return QStringLiteral("Destination must be an existing directory without symbolic links: %1").arg(systemReason(errno));
        ::close(fd);
        QString error;
        if (!tags.isEmpty() && !writeTagsXattr(paths.first(), tags, &error)) return error;
        return QString();
    }));
}

void IndexModel::finishArchive(bool ok, const QString &message, const QString &stage)
{
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, ok, message] {
        const QString error = watcher->result(); watcher->deleteLater();
        m_archive->complete(ok && error.isEmpty(), error.isEmpty() ? message : message + QStringLiteral("; ") + error);
    });
    watcher->setFuture(QtConcurrent::run([stage] {
        Store store(defaultIndexPath()); QString error;
        if (!store.lockAndLoad(&error)) return error;
        for (const Record &row : store.recordSnapshot())
            if (row.path == stage || isUnderDirectory(row.path, stage)) store.remove(row.path);
        if (!store.commit(&error)) return error;
        return QString();
    }));
}

void IndexModel::resolveConflict(const QString &policy, bool all)
{
    m_undo->resolveConflict(policy, all);
}

void IndexModel::undo()
{
    clearNotice();
    QPointer<IndexModel> self(this);
    m_undo->undo([self, receive = noticeReceiver()](bool, const QString &, const QString &receipt) {
        if (!self)
            return;
        self->reload();
        receive(receipt);
    });
}

// Every command line below reads the way it would be typed, `--` and all: the
// separator is not decoration here, it is what keeps a file called `-t` from
// being read as a flag, and the window has no say over what a folder is named.
void IndexModel::tagAdd(const QStringList &paths, const QString &tag)
{
    if (paths.isEmpty() || tag.isEmpty())
        return;
    report(QStringList{QStringLiteral("tag"), QStringLiteral("add"),
                       QStringLiteral("--tag"), tag, QStringLiteral("--")} + paths,
           QStringLiteral("tagged %1 %2").arg(countOf(paths.size(), QStringLiteral("file")), tag), true);
}

void IndexModel::tagRemove(const QStringList &paths, const QString &tag)
{
    if (paths.isEmpty() || tag.isEmpty())
        return;
    report(QStringList{QStringLiteral("tag"), QStringLiteral("rm"),
                       QStringLiteral("--tag"), tag, QStringLiteral("--")} + paths,
           QStringLiteral("untagged %1 %2").arg(countOf(paths.size(), QStringLiteral("file")), tag), true);
}

void IndexModel::tagRename(const QStringList &paths, const QString &from, const QString &to)
{
    if (paths.isEmpty() || from.isEmpty() || to.isEmpty())
        return;
    report(QStringList{QStringLiteral("tag"), QStringLiteral("rename"),
                       QStringLiteral("--"), from, to} + paths,
           QStringLiteral("Finished renaming tag %1 to %2 in the selection (%3)")
               .arg(from, to, countOf(paths.size(), QStringLiteral("file"))), true);
}

void IndexModel::tagClear(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    report(QStringList{QStringLiteral("tag"), QStringLiteral("clear"),
                       QStringLiteral("--")} + paths,
           QStringLiteral("cleared the tags on %1").arg(countOf(paths.size(),
                                                                QStringLiteral("file"))));
}

void IndexModel::tagCopy(const QString &from, const QStringList &paths)
{
    if (from.isEmpty() || paths.isEmpty())
        return;
    report(QStringList{QStringLiteral("tag"), QStringLiteral("cp"),
                       QStringLiteral("--from"), from, QStringLiteral("--")} + paths,
           QStringLiteral("copied the tags of %1 onto %2")
               .arg(QFileInfo(from).fileName(), countOf(paths.size(), QStringLiteral("file"))));
}

void IndexModel::adoptTagsIn(const QString &directory)
{
    if (directory.isEmpty())
        return;
    report({QStringLiteral("reindex"), QStringLiteral("--from-xattr"),
            QStringLiteral("--"), directory},
           QStringLiteral("adopted the tags on disk under %1").arg(directory));
}

// `--yes` because the question the command line asks is the one this menu entry
// already asked by being chosen. It skips the question and not the checks: a
// path that is not there, or a missing `gio`, still refuses the whole set.
void IndexModel::trash(const QStringList &paths)
{
    if (paths.isEmpty())
        return;
    report(QStringList{QStringLiteral("--yes"), QStringLiteral("trash"),
                       QStringLiteral("--")} + paths,
           QStringLiteral("trashed %1").arg(countOf(paths.size(), QStringLiteral("file"))));
}

void IndexModel::copyInto(const QStringList &paths, const QString &directory)
{
    if (paths.isEmpty() || directory.isEmpty())
        return;
    QVector<QStringList> runs;
    for (const QString &path : paths)
        runs.append({QStringLiteral("cp"), QStringLiteral("--"), path, directory});
    reportEach(runs, QStringLiteral("copied"));
}

void IndexModel::moveInto(const QStringList &paths, const QString &directory)
{
    if (paths.isEmpty() || directory.isEmpty())
        return;
    QVector<QStringList> runs;
    for (const QString &path : paths)
        runs.append({QStringLiteral("mv"), QStringLiteral("--"), path, directory});
    reportEach(runs, QStringLiteral("moved"));
}

// The name a copy gets, which the caller has to decide because `cp` refuses a
// destination that is already there rather than picking one. `report.pdf`
// becomes `report (copy).pdf` and then `report (copy 2).pdf`, the suffix going
// before the extension so the file is still a PDF to everything that reads one.
void IndexModel::duplicate(const QString &path)
{
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString()
                                                   : QLatin1Char('.') + info.suffix();
    const QDir dir(typedDirectory(path, nullptr));
    QString destination;
    for (int n = 1; n < 1000; ++n) {
        const QString tail = n == 1 ? QStringLiteral(" (copy)")
                                    : QStringLiteral(" (copy %1)").arg(n);
        const QString candidate = dir.filePath(stem + tail + suffix);
        if (!QFileInfo::exists(candidate)) {
            destination = candidate;
            break;
        }
    }
    if (destination.isEmpty()) {
        setNotice(QStringLiteral("duplicate: %1 already has a thousand copies").arg(info.fileName()));
        return;
    }
    report({QStringLiteral("cp"), QStringLiteral("--"), path, destination},
           QStringLiteral("copied to %1").arg(QFileInfo(destination).fileName()));
}

// A name that is taken is refused here rather than left to `mv`, which would
// not refuse it: handed a folder as the destination, `mv` moves the file
// inside, which is the right answer at a prompt and the wrong one for a box
// that asked for a new name. Asked of the name and not what it points at, so a
// dangling link counts as taken the way `mv` counts it.
void IndexModel::rename(const QString &path, const QString &to)
{
    if (path.isEmpty() || to.isEmpty() || path == to)
        return;
    const QFileInfo target(to);
    if (target.exists() || target.isSymLink()) {
        setNotice(QStringLiteral("mv: %1 already exists").arg(to));
        return;
    }
    report({QStringLiteral("mv"), QStringLiteral("--"), path, to},
           QStringLiteral("renamed %1 to %2").arg(QFileInfo(path).fileName(),
                                                   target.fileName()));
}

void IndexModel::renameSet(const QStringList &paths, const QString &pattern)
{
    if (paths.isEmpty() || pattern.isEmpty())
        return;
    report(QStringList{QStringLiteral("mv"), QStringLiteral("--pattern"), pattern,
                       QStringLiteral("--")} + paths,
           QStringLiteral("renamed %1").arg(countOf(paths.size(), QStringLiteral("file"))));
}

void IndexModel::changeMode(const QStringList &paths, const QString &mode)
{
    if (paths.isEmpty() || mode.isEmpty())
        return;
    report(QStringList{QStringLiteral("chmod"), mode, QStringLiteral("--")} + paths,
           QStringLiteral("set %1 on %2").arg(mode, countOf(paths.size(),
                                                              QStringLiteral("file"))));
}

void IndexModel::runScript(const QString &script, const QStringList &paths,
                           const QString &directory)
{
    if (script.isEmpty() || paths.isEmpty())
        return;
    QString error;
    if (!runUserScript(script, paths, directory, &error)) {
        setNotice(error);
        return;
    }
    setNotice(QStringLiteral("ran %1 over %2").arg(QFileInfo(script).fileName(),
                                                     countOf(paths.size(),
                                                             QStringLiteral("file"))));
}

// `mkdir(2)` and not `QDir::mkdir`, for the reason: the system says why it
// refused, and a folder that cannot be made in a place nobody may write to
// should say so rather than say nothing.
bool IndexModel::makeFolder(const QString &directory, const QString &name)
{
    if (directory.isEmpty() || name.isEmpty())
        return false;
    if (operationBusy()) {
        setNotice(QStringLiteral("An operation is still running; wait before creating a folder"));
        return false;
    }
    changeNoticeContext(m_noticeDirectory);
    const QString path = QDir(directory).filePath(name);
    if (::mkdir(QFile::encodeName(path).constData(), 0777) != 0) {
        setNotice(QStringLiteral("mkdir: cannot make %1: %2")
                      .arg(path, systemReason(errno)));
        return false;
    }
    m_undo->folderCreated(path);
    setNotice(QStringLiteral("made %1").arg(name));
    return true;
}

// Absolute targets preserve their meaning when the link changes folders.
namespace {
struct LinkResult {
    bool ok;
    QString notice;
};

LinkResult createLink(const QString &target, const QString &link)
{
    if (target.isEmpty() || link.isEmpty())
        return {false, {}};
    const QFileInfo at(link);
    if (at.exists() || at.isSymLink()) {
        return {false, QStringLiteral("ln: %1 already exists").arg(link)};
    }
    if (::symlink(QFile::encodeName(target).constData(),
                  QFile::encodeName(link).constData()) != 0) {
        return {false, QStringLiteral("ln: cannot link %1 to %2: %3")
                      .arg(link, target, systemReason(errno))};
    }
    return {true, QStringLiteral("linked %1 to %2").arg(at.fileName(), target)};
}
} // namespace

bool IndexModel::makeLink(const QString &target, const QString &link)
{
    changeNoticeContext(m_noticeDirectory);
    const auto result = createLink(target, link);
    setNotice(result.notice);
    return result.ok;
}

void IndexModel::linkInto(const QStringList &paths, const QString &directory)
{
    clearNotice();
    if (paths.isEmpty() || directory.isEmpty())
        return;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, receive = noticeReceiver()] {
        reload();
        receive(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([paths, directory] {
        int made = 0;
        QString error;
        for (const QString &path : paths) {
            const auto result = createLink(path, QDir(directory).filePath(QFileInfo(path).fileName()));
            if (result.ok)
                ++made;
            else if (error.isEmpty())
                error = result.notice;
        }
        return error.isEmpty() ? QStringLiteral("linked %1").arg(countOf(made, QStringLiteral("file")))
            : QStringLiteral("linked %1 of %2; %3").arg(made).arg(paths.size()).arg(error);
    }));
}

void IndexModel::reindexTree(const QString &directory)
{
    if (directory.isEmpty())
        return;
    report({QStringLiteral("reindex"), QStringLiteral("--"), directory},
           QStringLiteral("reindexed %1").arg(directory));
}

void IndexModel::pruneTree(const QString &directory)
{
    clearNotice();
    if (directory.isEmpty())
        return;
    QPointer<IndexModel> self(this);
    runVerb({QStringLiteral("--yes"), QStringLiteral("prune"), QStringLiteral("--"), directory},
            [self, directory, receive = noticeReceiver()](bool ok, const QString &output, const QString &error) {
        if (!self)
            return;
        if (!ok) {
            receive(error);
            return;
        }
        // Every line it printed is a row it dropped, so the count is the
        // receipt: `prune` on a clean tree prints nothing, and a menu entry
        // that says nothing back reads as one that did not run.
        const QStringList dropped = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        self->reload();
        receive(verbNotice(true, dropped.isEmpty()
                            ? QStringLiteral("nothing to prune under %1").arg(directory)
                            : QStringLiteral("pruned %1").arg(countOf(dropped.size(),
                                                                     QStringLiteral("row"))), error));
    });
}

// Like diff, verify exits 1 for findings. Keep its count and diagnostics together.
void IndexModel::verifyTree(const QString &directory)
{
    clearNotice();
    if (directory.isEmpty())
        return;
    QPointer<IndexModel> self(this);
    runVerb({QStringLiteral("verify"), QStringLiteral("--"), directory},
            [self, directory, receive = noticeReceiver()](bool ok, const QString &output, const QString &error) {
        if (!self)
            return;
        const QStringList problems = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!problems.isEmpty()) {
            receive(verbNotice(true, QStringLiteral("%1 under %2; omafiles verify says which")
                                .arg(countOf(problems.size(), QStringLiteral("problem")), directory), error));
            return;
        }
        receive(verbNotice(ok, QStringLiteral("%1 is clean").arg(directory), error));
    });
}

QString IndexModel::tagAt(int row) const
{
    if (!m_tags || row < 0 || row >= m_tags->rowCount())
        return {};
    return m_tags->data(m_tags->index(row, 0), TagListModel::TagRole).toString();
}

int IndexModel::tagRowCount() const
{
    return m_tags ? m_tags->rowCount() : 0;
}

} // namespace omafiles
