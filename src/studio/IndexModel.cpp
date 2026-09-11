#include "IndexModel.h"

#include "Open.h"
#include "Paths.h"
#include "Verb.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>

#include <cerrno>
#include <cstring>
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
        , m_owner(owner)
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

    void rebuild()
    {
        beginResetModel();
        QMap<QString, int> counts;
        for (const Record &record : m_owner->records()) {
            if (!statable(record.path, nullptr))
                continue;
            for (const QString &tag : record.tags)
                counts[tag] += 1;
        }
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
    IndexModel *m_owner;
    QVector<Row> m_tags;
};

IndexModel::IndexModel(QObject *parent)
    : QObject(parent)
    , m_store(defaultIndexPath())
{
    m_tags = new TagListModel(this);
    connect(this, &IndexModel::changed, m_tags, &TagListModel::rebuild);
    reload();

    // The directory has to exist before it can be watched, and on a machine
    // where the command line has never run, it does not. Both watches are taken
    // once, here, so without this a fresh install watches nothing and the
    // window does not follow the index until somebody presses F5. The studio
    // owns this directory as much as the CLI does.
    const QFileInfo info(m_store.indexPath());
    info.dir().mkpath(QStringLiteral("."));
    if (info.dir().exists())
        m_watcher.addPath(info.absolutePath());
    if (QFileInfo::exists(m_store.indexPath()))
        m_watcher.addPath(m_store.indexPath());

    m_settle.setSingleShot(true);
    m_settle.setInterval(60);
    connect(&m_settle, &QTimer::timeout, this, &IndexModel::reload);
    const auto later = [this] { m_settle.start(); };
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
    return m_store.get(path).tags;
}

void IndexModel::reload()
{
    QString error;
    const bool loaded = m_store.load(&error);

    // Tallied on every read, not only when the index is written: what the
    // system says about a row changes when a drive is mounted or a permission
    // is set, and the index does not move for either.
    m_present = 0;
    m_missing = 0;
    m_unknown = 0;
    m_unreachable = 0;
    QString declined;
    QString waiting;
    for (const Record &record : m_store.records()) {
        QString detail;
        switch (pathState(record.path, &detail)) {
        case PathState::Present:
            ++m_present;
            break;
        case PathState::Missing:
            ++m_missing;
            break;
        case PathState::Unknown:
            ++m_unknown;
            if (declined.isEmpty())
                declined = detail;
            break;
        // A row on a disk nobody has plugged in. Counted apart from the two
        // above because it is the one an answer is expected for: the drive
        // comes back and so do the rows, and nothing here or in the command
        // line will touch them meanwhile.
        case PathState::Unreachable:
            ++m_unreachable;
            if (waiting.isEmpty())
                waiting = detail;
            break;
        }
    }

    // The counts are of rows the system will describe. A count that came up
    // short is not the answer that was asked for, so it says so -- the CLI
    // spends an exit code on this and the window has only this line. A row that
    // is simply gone is not worth a permanent alarm on its own, but an index
    // with rows in it and nothing reachable is: the zeros above are a lie about
    // an index that is not empty.
    if (!loaded)
        m_error = error;
    else if (m_unreachable > 0 && m_present == 0)
        m_error = QStringLiteral("%1 index row(s) are on a root that is not here; %2")
                      .arg(m_unreachable).arg(waiting);
    else if (m_unknown > 0)
        m_error = QStringLiteral("%1 index row(s) not counted; %2").arg(m_unknown).arg(declined);
    else if (m_missing > 0 && m_present == 0)
        m_error = QStringLiteral("%1 index row(s), and none of their files are there; "
                                 "omafiles prune drops them").arg(m_missing);
    else
        m_error.clear();
    // A commit lands by rename, which takes the watch on the old file with it,
    // so the file watch is re-taken on every read rather than only at startup.
    if (QFileInfo::exists(m_store.indexPath())
        && !m_watcher.files().contains(m_store.indexPath()))
        m_watcher.addPath(m_store.indexPath());
    emit changed();
}

void IndexModel::openPath(const QString &path)
{
    QString error;
    if (!openLocalPath(path, &error)) {
        m_error = error;
        emit changed();
    }
}

bool IndexModel::canShare() const
{
    return canShareLocalSend();
}

void IndexModel::sharePath(const QString &path)
{
    QString error;
    if (!shareLocalPath(path, &error)) {
        m_error = error;
        emit changed();
    }
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


void IndexModel::setNotice(const QString &message)
{
    if (m_notice == message)
        return;
    m_notice = message;
    emit noticeChanged();
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

void IndexModel::report(const QStringList &arguments, const QString &success)
{
    QPointer<IndexModel> self(this);
    runVerb(arguments, [self, success](bool ok, const QString &, const QString &error) {
        if (!self)
            return;
        self->setNotice(ok ? success : error);
        // Re-read rather than wait for the watcher: the watch is coalesced by a
        // timer, so without this the receipt would be on screen for a moment
        // beside a list that still shows the tags the run has just changed.
        if (ok)
            self->reload();
    });
}

void IndexModel::reportEach(const QVector<QStringList> &runs, const QString &verb)
{
    if (runs.isEmpty())
        return;

    struct Tally {
        QVector<QStringList> runs;
        int at = 0;
        int done = 0;
        QString firstError;
    };
    auto tally = std::make_shared<Tally>();
    tally->runs = runs;

    QPointer<IndexModel> self(this);
    auto step = std::make_shared<std::function<void()>>();
    *step = [self, tally, step, verb] {
        if (tally->at >= tally->runs.size()) {
            if (self) {
                const int asked = tally->runs.size();
                self->setNotice(tally->firstError.isEmpty()
                                    ? QStringLiteral("%1 %2").arg(verb, countOf(tally->done,
                                          QStringLiteral("file")))
                                    : QStringLiteral("%1 %2 of %3; %4")
                                          .arg(verb).arg(tally->done).arg(asked)
                                          .arg(tally->firstError));
                if (tally->done > 0)
                    self->reload();
            }
            // The step holds itself so it can call itself; letting go here is
            // what keeps the cycle from outliving the run.
            *step = nullptr;
            return;
        }
        const QStringList arguments = tally->runs.at(tally->at++);
        runVerb(arguments, [tally, step](bool ok, const QString &, const QString &error) {
            if (ok)
                ++tally->done;
            else if (tally->firstError.isEmpty())
                tally->firstError = error;
            (*step)();
        });
    };
    (*step)();
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
           QStringLiteral("tagged %1 %2").arg(countOf(paths.size(), QStringLiteral("file")), tag));
}

void IndexModel::tagRemove(const QStringList &paths, const QString &tag)
{
    if (paths.isEmpty() || tag.isEmpty())
        return;
    report(QStringList{QStringLiteral("tag"), QStringLiteral("rm"),
                       QStringLiteral("--tag"), tag, QStringLiteral("--")} + paths,
           QStringLiteral("untagged %1 %2").arg(countOf(paths.size(), QStringLiteral("file")), tag));
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
    const QDir dir(info.absolutePath());
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
    const QString path = QDir(directory).filePath(name);
    if (::mkdir(QFile::encodeName(path).constData(), 0777) != 0) {
        setNotice(QStringLiteral("mkdir: cannot make %1: %2")
                      .arg(path, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    setNotice(QStringLiteral("made %1").arg(name));
    return true;
}

// Refused where something is already there, in the words `mv` refuses with,
// and asked of the name and not what it points at, so a dangling link counts.
// The target is written as the absolute path it was handed: a link that
// still points at the same file after it is moved is worth more than a
// shorter one.
bool IndexModel::makeLink(const QString &target, const QString &link)
{
    if (target.isEmpty() || link.isEmpty())
        return false;
    const QFileInfo at(link);
    if (at.exists() || at.isSymLink()) {
        setNotice(QStringLiteral("ln: %1 already exists").arg(link));
        return false;
    }
    if (::symlink(QFile::encodeName(target).constData(),
                  QFile::encodeName(link).constData()) != 0) {
        setNotice(QStringLiteral("ln: cannot link %1 to %2: %3")
                      .arg(link, target, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    setNotice(QStringLiteral("linked %1 to %2").arg(at.fileName(), target));
    return true;
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
    if (directory.isEmpty())
        return;
    QPointer<IndexModel> self(this);
    runVerb({QStringLiteral("--yes"), QStringLiteral("prune"), QStringLiteral("--"), directory},
            [self, directory](bool ok, const QString &output, const QString &error) {
        if (!self)
            return;
        if (!ok) {
            self->setNotice(error);
            return;
        }
        // Every line it printed is a row it dropped, so the count is the
        // receipt: `prune` on a clean tree prints nothing, and a menu entry
        // that says nothing back reads as one that did not run.
        const QStringList dropped = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        self->setNotice(dropped.isEmpty()
                            ? QStringLiteral("nothing to prune under %1").arg(directory)
                            : QStringLiteral("pruned %1").arg(countOf(dropped.size(),
                                                                     QStringLiteral("row"))));
        self->reload();
    });
}

// The one verb here that reports by exiting 1 without having failed: `verify`
// says it found something the way `diff` does. So the count is the answer, and
// the stderr is only read when there was no answer to read.
void IndexModel::verifyTree(const QString &directory)
{
    if (directory.isEmpty())
        return;
    QPointer<IndexModel> self(this);
    runVerb({QStringLiteral("verify"), QStringLiteral("--"), directory},
            [self, directory](bool ok, const QString &output, const QString &error) {
        if (!self)
            return;
        const QStringList problems = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!problems.isEmpty()) {
            self->setNotice(QStringLiteral("%1 under %2; omafiles verify says which")
                                .arg(countOf(problems.size(), QStringLiteral("problem")), directory));
            return;
        }
        self->setNotice(ok ? QStringLiteral("%1 is clean").arg(directory) : error);
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
