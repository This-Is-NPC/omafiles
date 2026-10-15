#include "UndoJournal.h"

#include "Paths.h"
#include "Store.h"
#include "Trash.h"
#include "Xattr.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QStorageInfo>
#include <QtConcurrentRun>

#include <algorithm>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace omafiles {
namespace {

template<typename Result, typename Work, typename Done>
void background(QObject *owner, Work work, Done done)
{
    auto *watcher = new QFutureWatcher<Result>(owner);
    QObject::connect(watcher, &QFutureWatcher<Result>::finished, owner, [watcher, done] {
        const Result result = watcher->result();
        watcher->deleteLater();
        done(result);
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

bool inside(const QString &path, const QStringList &scope)
{
    return std::any_of(scope.cbegin(), scope.cend(), [&](const QString &root) {
        return path == root || isUnderDirectory(path, root);
    });
}

QMap<QString, Record> rowsIn(const QMap<QString, Record> &rows, const QStringList &scope)
{
    QMap<QString, Record> result;
    for (const Record &row : rows) {
        if (inside(row.path, scope))
            result.insert(row.path, row);
    }
    return result;
}

QMap<QString, Record> rowsIn(const Store &store, const QStringList &scope)
{
    return rowsIn(store.recordSnapshot(), scope);
}

QString named(const QString &path)
{
    QString name;
    const QString parent = canonicalPath(typedDirectory(path, &name), nullptr);
    return parent.isEmpty() ? QString() : QDir(parent).filePath(name);
}

QString findTrashed(const QString &original, quint64 device, quint64 inode, QString *info, const QSet<QString> &excluded = {})
{
    const QString mount = QStorageInfo(typedDirectory(original, nullptr)).rootPath();
    const auto listing = Trash::list(Trash::mounts().contains(mount) ? QStringList{mount} : QStringList{});
    for (const auto &item : listing.items) {
        if (item.error.isEmpty() && item.original == original
            && item.fileDevice == device && item.fileInode == inode && !excluded.contains(item.info)) {
            *info = item.info;
            return item.path;
        }
    }
    return {};
}

QByteArray birthOf(const QString &path)
{
    struct statx found {};
    if (::statx(AT_FDCWD, QFile::encodeName(path).constData(), AT_SYMLINK_NOFOLLOW,
                STATX_BTIME, &found) != 0 || !(found.stx_mask & STATX_BTIME))
        return {};
    return QByteArray::number(found.stx_btime.tv_sec) + ':' + QByteArray::number(found.stx_btime.tv_nsec);
}

QByteArray hashFile(const QString &path, QString *error)
{
    QFile file;
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        *error = QStringLiteral("cannot read %1: %2").arg(spelled(path), systemReason(errno));
        return {};
    }
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        *error = QStringLiteral("cannot read %1: %2").arg(spelled(path), file.errorString());
        return {};
    }
    return hash.result();
}

} // namespace

UndoJournal::UndoJournal(QObject *parent) : QObject(parent) {}

UndoJournal::~UndoJournal()
{
    const QVector<Step> saved = m_saved;
    if (!saved.isEmpty())
        (void)QtConcurrent::run([saved] { discard(saved); });
}

void UndoJournal::discard(const QVector<Step> &steps)
{
    QString error;
    Store store(defaultIndexPath());
    if (!store.lockAndLoad(&error)) return;
    bool changed = false;
    for (const Step &step : steps) {
        error.clear();
        if (!step.temporaryBackup || named(step.to) != step.to) continue;
        const State expected = step.guards.value(step.to);
        const State now = fingerprint(step.to, &error);
        if (!error.isEmpty() || now.device != expected.device || now.inode != expected.inode
            || now.birth != expected.birth || now.digest != expected.digest) continue;
        const QStringList scope{step.to};
        if (rowsIn(store, scope) != rowsIn(step.afterRows, scope)) continue;
        const QByteArray bytes = QFile::encodeName(step.to);
        struct stat st {};
        if (::lstat(bytes.constData(), &st) != 0) continue;
        if ((S_ISDIR(st.st_mode) ? ::rmdir(bytes.constData()) : ::unlink(bytes.constData())) != 0) continue;
        for (const Record &row : rowsIn(store, scope)) store.remove(row.path);
        changed = true;
    }
    if (changed) store.commit(&error);
}

QString UndoJournal::label() const
{
    return m_entries.isEmpty() ? QStringLiteral("Undo")
                              : QStringLiteral("Undo %1").arg(m_entries.last().label);
}

void UndoJournal::setBusy(bool busy)
{
    m_busy = busy;
    emit changed();
}

UndoJournal::State UndoJournal::fingerprint(const QString &path, QString *error, bool emptyFolder)
{
    State result;
    if (named(path) != path) {
        *error = QStringLiteral("the parent of %1 changed or cannot be read").arg(spelled(path));
        return result;
    }
    struct stat st {};
    if (statState(path, error, false, &st) != PathState::Present) {
        if (error->isEmpty())
            *error = QStringLiteral("%1 is missing").arg(spelled(path));
        return result;
    }
    result.device = st.st_dev;
    result.inode = st.st_ino;
    result.birth = birthOf(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto add = [&hash](const QByteArray &value) {
        hash.addData(QByteArray::number(value.size()));
        hash.addData(QByteArrayLiteral(":"));
        hash.addData(value);
    };
    add(QByteArray::number(st.st_mode));
    add(QByteArray::number(st.st_uid));
    add(QByteArray::number(st.st_gid));
    if (S_ISREG(st.st_mode)) {
        add(QByteArray::number(st.st_size));
        add(QByteArray::number(st.st_mtim.tv_sec));
        add(QByteArray::number(st.st_mtim.tv_nsec));
        add(hashFile(path, error));
    } else if (S_ISLNK(st.st_mode)) {
        const QByteArray target = linkTarget(QFile::encodeName(path));
        if (target.isEmpty())
            *error = QStringLiteral("cannot read link %1").arg(spelled(path));
        add(target);
    } else if (S_ISDIR(st.st_mode)) {
        QStringList names, unnameable;
        if (!readDirectory(path, &names, &unnameable, error))
            return {};
        if (!unnameable.isEmpty())
            *error = QStringLiteral("%1 contains names that are not UTF-8").arg(spelled(path));
        if (emptyFolder && !names.isEmpty())
            *error = QStringLiteral("%1 is no longer empty").arg(spelled(path));
        std::sort(names.begin(), names.end());
        for (const QString &name : names) {
            const State child = fingerprint(QDir(path).filePath(name), error);
            if (!error->isEmpty())
                return {};
            add(name.toUtf8());
            add(QByteArray::number(child.device));
            add(QByteArray::number(child.inode));
            add(child.birth);
            add(child.digest);
        }
    } else {
        *error = QStringLiteral("%1 is not a regular file, folder or symbolic link").arg(spelled(path));
    }
    QByteArray value;
    if (readTagsValue(path, &value, error) == TagsValue::Unknown)
        return {};
    add(value);
    struct stat after {};
    if (::lstat(QFile::encodeName(path).constData(), &after) != 0
        || after.st_dev != st.st_dev || after.st_ino != st.st_ino
        || after.st_ctim.tv_sec != st.st_ctim.tv_sec || after.st_ctim.tv_nsec != st.st_ctim.tv_nsec)
        *error = QStringLiteral("%1 changed while being checked; retry Undo").arg(spelled(path));
    result.digest = hash.result();
    return result;
}

UndoJournal::Before UndoJournal::capture(const QStringList &arguments)
{
    Before before;
    const QString verb = journalVerb(arguments);
    if (verb != QLatin1String("mv") && verb != QLatin1String("trash"))
        return before;
    QStringList paths = arguments.mid(arguments.indexOf(QStringLiteral("--")) + 1);
    if (verb == QLatin1String("mv") && !arguments.contains(QStringLiteral("--pattern")))
        paths = paths.mid(0, 1);
    for (const QString &typed : paths) {
        const QString path = named(typed);
        if (path.isEmpty())
            continue;
        {
            struct stat st {};
            if (::lstat(QFile::encodeName(path).constData(), &st) == 0) {
                State state;
                state.device = st.st_dev;
                state.inode = st.st_ino;
                state.birth = birthOf(path);
                before.files.insert(path, state);
                if (verb == QLatin1String("trash")) {
                    QString info;
                    while (!findTrashed(path, state.device, state.inode, &info, before.trashSeen).isEmpty())
                        before.trashSeen.insert(info);
                }
            }
        }
    }
    return before;
}

QVector<UndoJournal::Step> UndoJournal::completed(const QStringList &arguments, const QString &output,
                                                const Before &captured, QString *error)
{
    QVector<Step> steps;
    Before before = captured;
    QJsonDocument receipt = jsonDocument(output.toUtf8(), nullptr);
    QMap<QString, Record> actualAfter;
    QMap<QString, QByteArray> expectedValues;
    if (output.trimmed().isEmpty())
        return steps;
    if (!receipt.object().contains(QStringLiteral("journal"))) {
        *error = QStringLiteral("the command did not provide its journal receipt");
        return steps;
    }
    {
        const QJsonObject journal = receipt.object().value(QStringLiteral("journal")).toObject();
        before.error = journal.value(QStringLiteral("error")).toString();
        const auto records = [error](const QJsonArray &array) {
            QMap<QString, Record> rows;
            for (const QJsonValue &value : array) {
                const Record row = Record::fromJson(value.toObject(), error);
                if (!row.path.isEmpty()) rows.insert(row.path, row);
            }
            return rows;
        };
        before.rows = records(journal.value(QStringLiteral("beforeRows")).toArray());
        actualAfter = records(journal.value(QStringLiteral("afterRows")).toArray());
        before.tags.clear();
        const QJsonObject values = journal.value(QStringLiteral("beforeValues")).toObject();
        for (auto it = values.begin(); it != values.end(); ++it)
            before.tags.insert(it.key(), Tags{before.rows.value(it.key()), QByteArray::fromBase64(it.value().toString().toLatin1())});
        const QJsonObject after = journal.value(QStringLiteral("afterValues")).toObject();
        for (auto it = after.begin(); it != after.end(); ++it)
            expectedValues.insert(it.key(), QByteArray::fromBase64(it.value().toString().toLatin1()));
        const QJsonValue result = receipt.object().value(QStringLiteral("result"));
        receipt = result.isArray() ? QJsonDocument(result.toArray()) : QJsonDocument(result.toObject());
    }
    *error = before.error;
    if (!error->isEmpty())
        return steps;
    Store store(defaultIndexPath());
    if (!store.load(error))
        return steps;
    const QString verb = journalVerb(arguments);
    if (verb == QLatin1String("tag")) {
        for (auto it = before.tags.cbegin(); it != before.tags.cend(); ++it) {
            QByteArray now;
            if (readTagsValue(it.key(), &now, error) == TagsValue::Unknown)
                return {};
            if (now != expectedValues.value(it.key())) {
                *error = QStringLiteral("%1 changed while its operation was finishing; its tags were left untouched").arg(spelled(it.key()));
                return {};
            }
            if (actualAfter.value(it.key()) == it->row && now == it->value)
                continue;
            Step step;
            step.kind = verb;
            step.to = it.key();
            steps.append(step);
        }
    } else if (verb == QLatin1String("mv") || verb == QLatin1String("cp")) {
        const QJsonArray moves = receipt.isArray() ? receipt.array() : QJsonArray{receipt.object()};
        for (const QJsonValue &value : moves) {
            const QJsonObject move = value.toObject();
            Step step;
            step.kind = move.value(QStringLiteral("kind")).toString(verb);
            step.temporaryBackup = move.value(QStringLiteral("temporaryBackup")).toBool();
            step.from = move.value(QStringLiteral("from")).toString();
            step.to = move.value(QStringLiteral("to")).toString();
            if (!step.from.isEmpty() && !step.to.isEmpty() && step.from != step.to)
                steps.append(step);
        }
    } else if (verb == QLatin1String("trash")) {
        for (const QJsonValue &removed : receipt.object().value(QStringLiteral("removed")).toArray()) {
            const QString original = removed.toString();
            if (!before.files.contains(original))
                continue;
            const QJsonArray removedPaths = receipt.object().value(QStringLiteral("removed")).toArray();
            const bool nested = std::any_of(removedPaths.cbegin(), removedPaths.cend(), [&](const QJsonValue &other) {
                return original != other.toString() && before.files.contains(other.toString())
                    && isUnderDirectory(original, other.toString());
            });
            if (nested)
                continue;
            const State own = before.files.value(original);
            Step step;
            step.kind = verb;
            step.from = original;
            step.to = findTrashed(original, own.device, own.inode, &step.info, before.trashSeen);
            if (step.to.isEmpty()) {
                *error = QStringLiteral("cannot identify %1 in the local Trash; restore it in your Trash browser")
                             .arg(spelled(original));
                return {};
            }
            step.infoDigest = hashFile(step.info, error);
            steps.append(step);
        }
    }
    QMap<QString, Record> intermediate = before.rows;
    const QJsonArray moves = receipt.isArray() ? receipt.array() : QJsonArray{receipt.object()};
    for (qsizetype at = 0; at < steps.size(); ++at) {
        Step &step = steps[at];
        step.scope = {step.to};
        if (!step.from.isEmpty() && step.kind != QLatin1String("cp"))
            step.scope.append(step.from);
        for (qsizetype earlier = 0; earlier < at; ++earlier)
            if (steps.at(earlier).temporaryBackup) step.scope.append(steps.at(earlier).to);
        step.beforeRows = rowsIn(intermediate, step.scope);
        QString finalPath = step.to;
        if (verb == QLatin1String("mv") || verb == QLatin1String("cp")) {
            for (auto it = intermediate.begin(); it != intermediate.end();) {
                if (it.key() == step.to || isUnderDirectory(it.key(), step.to)
                    || (step.kind != QLatin1String("cp") && (it.key() == step.from || isUnderDirectory(it.key(), step.from))))
                    it = intermediate.erase(it);
                else
                    ++it;
            }
            for (const QJsonValue &value : moves.at(at).toObject().value(QStringLiteral("rows")).toArray()) {
                QString why;
                const Record row = Record::fromJson(value.toObject(), &why);
                if (!row.path.isEmpty() && !row.tags.isEmpty())
                    intermediate.insert(row.path, row);
            }
            step.afterRows = rowsIn(intermediate, step.scope);
            for (qsizetype later = at + 1; later < steps.size(); ++later) {
                const Step &move = steps.at(later);
                if (finalPath == move.from || isUnderDirectory(finalPath, move.from))
                    finalPath = finalPath == move.from ? move.to
                        : QDir(move.to).filePath(relativeTo(finalPath, move.from));
            }
        } else {
            step.afterRows = rowsIn(actualAfter, step.scope);
        }
        const QString source = step.from.isEmpty() ? step.to : step.from;
        if (step.kind != QLatin1String("cp") && before.tags.contains(source))
            step.beforeValues.insert(source, before.tags.value(source).value);
        step.original = before.files.value(source);
        step.guards.insert(step.to, fingerprint(finalPath, error));
        for (qsizetype earlier = 0; earlier < at; ++earlier) {
            const Step &saved = steps.at(earlier);
            if (saved.temporaryBackup)
                step.guards.insert(saved.to, fingerprint(saved.to, error));
        }
        if (step.temporaryBackup) step.original = step.guards.value(step.to);
        if (!error->isEmpty())
            return {};
    }
    if (verb == QLatin1String("tag") && steps.size() > 1) {
        Step combined = steps.takeFirst();
        for (const Step &step : steps) {
            combined.scope += step.scope;
            combined.beforeRows.insert(step.beforeRows);
            combined.afterRows.insert(step.afterRows);
            combined.beforeValues.insert(step.beforeValues);
            combined.guards.insert(step.guards);
        }
        steps = {combined};
    }
    return steps;
}

void UndoJournal::append(Entry entry)
{
    if (entry.steps.isEmpty() && entry.refusal.isEmpty())
        return;
    for (const Step &step : entry.steps)
        if (step.temporaryBackup) m_saved.append(step);
    qsizetype size = 0;
    for (const Step &step : entry.steps) {
        for (const Record &row : step.beforeRows)
            size += QJsonDocument(row.toJson()).toJson(QJsonDocument::Compact).size();
        for (const Record &row : step.afterRows)
            size += QJsonDocument(row.toJson()).toJson(QJsonDocument::Compact).size();
        for (auto it = step.beforeValues.cbegin(); it != step.beforeValues.cend(); ++it)
            size += it.key().size() * 2 + it.value().size();
        size += (step.to.size() + step.from.size()) * 2 + 256;
    }
    if (size > 1024 * 1024) {
        entry.steps.clear();
        entry.refusal = QStringLiteral("this operation exceeds the 1 MiB journal entry limit");
    }
    m_entries.append(std::move(entry));
    while (m_entries.size() > 50) {
        const Entry expired = m_entries.takeFirst();
        for (const Step &step : expired.steps) {
            if (!step.temporaryBackup) continue;
            m_saved.erase(std::remove_if(m_saved.begin(), m_saved.end(), [&](const Step &saved) {
                return saved.to == step.to;
            }), m_saved.end());
        }
        (void)QtConcurrent::run([expired] { discard(expired.steps); });
    }
    emit changed();
}

void UndoJournal::run(const QVector<QStringList> &commands, const QString &label, VerbReceipt done, bool askConflicts)
{
    run(commands, label, std::move(done), askConflicts, {}, {}, {});
}

void UndoJournal::install(const QStringList &paths, const QString &directory, const QString &label,
                          VerbReceipt done, std::function<bool()> cancelled, std::function<void(int, int)> progress)
{
    QVector<QStringList> commands;
    for (const QString &path : paths)
        commands.append({QStringLiteral("mv"), QStringLiteral("-T"), QStringLiteral("--"), path,
                         QDir(directory).filePath(QFileInfo(path).fileName())});
    run(commands, label, std::move(done), true, paths, std::move(cancelled), std::move(progress));
}

void UndoJournal::run(const QVector<QStringList> &commands, const QString &label, VerbReceipt done,
                      bool askConflicts, const QStringList &installedSources, std::function<bool()> cancelled,
                      std::function<void(int, int)> progress)
{
    if (m_busy) {
        done(false, {}, QStringLiteral("An operation is still running; wait for its receipt before trying again"));
        return;
    }
    setBusy(true);
    struct Run {
        QVector<QStringList> commands;
        Entry entry;
        int at = 0;
        bool ok = true;
        int succeeded = 0;
        QStringList notes;
        QString policy;
        bool cancelled = false;
    };
    auto run = std::make_shared<Run>();
    run->commands = commands;
    run->entry.label = label;
    auto next = std::make_shared<std::function<void()>>();
    QPointer<UndoJournal> self(this);
    if (progress) progress(0, commands.size());
    *next = [self, run, next, done, askConflicts, installedSources, cancelled, progress] {
        if (!self) { *next = nullptr; return; }
        if (cancelled && cancelled() && !run->cancelled) {
            run->at = run->commands.size();
            run->ok = false;
            run->cancelled = true;
            run->notes.append(QStringLiteral("Cancelled; completed files can be undone"));
        }
        if (run->at == run->commands.size()) {
            self->append(run->entry);
            self->setBusy(false);
            done(run->ok, QString::number(run->succeeded), run->notes.join(QStringLiteral("; ")));
            *next = nullptr;
            return;
        }
        const QStringList original = run->commands.at(run->at++);
        const auto execute = [self, run, next, installedSources, cancelled, progress](const QStringList &args) {
            if (cancelled && cancelled()) { (*next)(); return; }

            background<Before>(self, [args] { return capture(args); }, [self, run, next, args, installedSources, cancelled, progress](const Before &before) {
                if (!self) { *next = nullptr; return; }
                if (cancelled && cancelled()) { (*next)(); return; }
                self->m_waitingForVerb = true;
                runVerb(args, [self, run, next, args, before, installedSources, progress](bool ok, const QString &output, const QString &note) {
                    if (!self) { *next = nullptr; return; }
                    self->m_waitingForVerb = false;
                    run->ok = run->ok && ok;
                    run->succeeded += ok ? 1 : 0;
                    if (!note.isEmpty())
                        run->notes.append(note);
                    background<Entry>(self, [args, output, before, installedSources] {
                        Entry entry;
                        entry.steps = completed(args, output, before, &entry.refusal);
                        for (Step &step : entry.steps) {
                            if (!step.temporaryBackup && inside(step.from, installedSources)) {
                                step.kind = QStringLiteral("created");
                                step.from.clear();
                            }
                            step.scope.erase(std::remove_if(step.scope.begin(), step.scope.end(), [&](const QString &path) {
                                return inside(path, installedSources);
                            }), step.scope.end());
                            const auto forgetSources = [&](auto &values) {
                                for (auto it = values.begin(); it != values.end();) {
                                    if (inside(it.key(), installedSources)) it = values.erase(it);
                                    else ++it;
                                }
                            };
                            forgetSources(step.beforeRows);
                            forgetSources(step.afterRows);
                            forgetSources(step.beforeValues);
                            forgetSources(step.guards);
                        }
                        return entry;
                    }, [run, next, progress](const Entry &entry) {
                        run->entry.steps += entry.steps;
                        if (!entry.refusal.isEmpty()) {
                            run->entry.refusal = entry.refusal;
                            run->notes.append(QStringLiteral("Undo unavailable: %1").arg(entry.refusal));
                        }
                        if (progress) progress(run->succeeded, run->commands.size());
                        (*next)();
                    });
                });
            });
        };
        struct Landing { QString from, to, error; bool occupied = false; };
        background<Landing>(self, [original, askConflicts] {
            Landing landing;
            if (!askConflicts) return landing;
            const QString verb = journalVerb(original);
            if ((verb != QLatin1String("cp") && verb != QLatin1String("mv"))
                || original.contains(QStringLiteral("--pattern"))
                || original.contains(QStringLiteral("--force")) || original.contains(QStringLiteral("--keep-both"))
                || original.contains(QStringLiteral("--backup")) || original.contains(QStringLiteral("--no-clobber"))) return landing;
            const QStringList paths = original.mid(original.indexOf(QStringLiteral("--")) + 1);
            if (paths.size() != 2) return landing;
            landing.from = paths.first();
            const QString target = paths.last();
            landing.to = original.contains(QStringLiteral("-T")) ? target
                : QDir(target).filePath(QFileInfo(landing.from).fileName());
            // A typed file destination is already exact; directory receivers add the basename.
            struct stat directory {};
            if (!original.contains(QStringLiteral("-T"))
                && (statState(target, nullptr, true, &directory) != PathState::Present || !S_ISDIR(directory.st_mode)))
                landing.to = target;
            const PathState state = statState(landing.to, &landing.error, false);
            landing.occupied = state == PathState::Present;
            if (state == PathState::Missing) landing.error.clear();
            return landing;
        }, [self, run, next, original, execute, installedSources, cancelled](const Landing &landing) {
            if (!self) { *next = nullptr; return; }
            if (cancelled && cancelled()) { (*next)(); return; }
            if (!landing.error.isEmpty()) {
                run->ok = false;
                run->notes.append(landing.error);
                (*next)();
                return;
            }
            if (!landing.occupied) { execute(original); return; }
            const auto decide = [self, run, next, original, landing, execute, installedSources](const QString &policy, bool all) {
                if (all) run->policy = policy;
                if (policy == QLatin1String("cancel")) {
                    run->at = run->commands.size();
                    if (!installedSources.isEmpty()) run->ok = false;
                    run->cancelled = true;
                    run->notes.append(QStringLiteral("Cancelled; completed files can be undone"));
                    (*next)();
                } else if (policy == QLatin1String("skip")) {
                    run->notes.append(QStringLiteral("Skipped %1").arg(spelled(landing.to)));
                    (*next)();
                } else {
                    QStringList args = original.mid(0, original.indexOf(QStringLiteral("--")));
                    args << QStringLiteral("-T")
                         << (policy == QLatin1String("replace") ? QStringLiteral("--force") : QStringLiteral("--keep-both"))
                         << QStringLiteral("--") << landing.from << landing.to;
                    execute(args);
                }
            };
            if (!run->policy.isEmpty()) { decide(run->policy, true); return; }
            self->m_conflict = decide;
            emit self->conflictRequested(landing.from, landing.to);
        });
    };
    (*next)();
}

void UndoJournal::resolveConflict(const QString &policy, bool all)
{
    if (policy != QLatin1String("replace") && policy != QLatin1String("skip")
        && policy != QLatin1String("keep") && policy != QLatin1String("cancel")) return;
    auto decide = std::move(m_conflict);
    m_conflict = {};
    if (decide) decide(policy, all);
}

void UndoJournal::folderCreated(const QString &path)
{
    setBusy(true);
    background<Entry>(this, [path] {
        Step step;
        step.kind = QStringLiteral("mkdir");
        step.to = named(path);
        step.scope = {step.to};
        Entry entry;
        entry.label = QStringLiteral("new folder");
        step.guards.insert(step.to, fingerprint(step.to, &entry.refusal, true));
        Store store(defaultIndexPath());
        if (store.load(&entry.refusal)) {
            step.beforeRows = rowsIn(store, step.scope);
            step.afterRows = step.beforeRows;
        }
        entry.steps = {step};
        return entry;
    }, [this](const Entry &entry) { append(entry); setBusy(false); });
}

QString UndoJournal::check(const Step &step)
{
    QString error;
    for (auto it = step.guards.cbegin(); it != step.guards.cend(); ++it) {
        const State now = fingerprint(it.key(), &error, step.kind == QLatin1String("mkdir"));
        if (!error.isEmpty())
            return error;
        if (now.device != it->device || now.inode != it->inode || now.birth != it->birth || now.digest != it->digest)
            return QStringLiteral("%1 changed since the operation; it was left untouched").arg(spelled(it.key()));
    }
    if (step.kind == QLatin1String("rows")) {
        if (statState(step.to, &error, false) != PathState::Missing)
            return QStringLiteral("%1 is occupied; it was left untouched").arg(spelled(step.to));
        error.clear();
    }
    if (step.kind == QLatin1String("mv") || step.kind == QLatin1String("trash")) {
        if (statState(step.from, &error, false) != PathState::Missing)
            return error.isEmpty() ? QStringLiteral("%1 is occupied; nothing will be overwritten").arg(spelled(step.from)) : error;
        error.clear();
        if (named(step.from) != step.from)
            return QStringLiteral("the original parent of %1 changed or is missing").arg(spelled(step.from));
    }
    Store store(defaultIndexPath());
    if (!store.load(&error))
        return error;
    if (rowsIn(store, step.scope) != step.afterRows)
        return QStringLiteral("the indexed tags changed since the operation; they were left untouched");
    if (!step.info.isEmpty() && (named(step.info) != step.info || hashFile(step.info, &error) != step.infoDigest))
        return QStringLiteral("the Trash record changed or cannot be read; it was left untouched");
    return error;
}

QStringList UndoJournal::inverse(const Step &step)
{
    if (step.kind == QLatin1String("mv") || step.kind == QLatin1String("trash"))
        return {QStringLiteral("mv"), QStringLiteral("-T"), QStringLiteral("--"), step.to, step.from};
    if (step.kind == QLatin1String("cp"))
        return {QStringLiteral("--yes"), QStringLiteral("rm"), QStringLiteral("--"), step.to};
    return {};
}

QString UndoJournal::restoreTags(const Step &step)
{
    QString error;
    Store store(defaultIndexPath());
    if (!store.lockAndLoad(&error))
        return error;
    error = check(step);
    if (!error.isEmpty())
        return error;
    struct Value { int fd; QByteArray before; QByteArray after; };
    QVector<Value> values;
    const auto close = [&values] { for (const Value &value : values) ::close(value.fd); };
    const auto rollback = [&values] {
        QString reason;
        for (const Value &value : values)
            writeTagsValue(value.fd, value.after, &reason);
    };
    for (auto it = step.beforeValues.cbegin(); it != step.beforeValues.cend(); ++it) {
        if (named(it.key()) != it.key()) { close(); return QStringLiteral("the original parent changed"); }
        struct stat st {};
        if (::lstat(QFile::encodeName(it.key()).constData(), &st) != 0) {
            close(); return systemReason(errno);
        }
        if (S_ISLNK(st.st_mode))
            continue;
        QByteArray current;
        if (readTagsValue(it.key(), &current, &error) == TagsValue::Unknown) { close(); return error; }
        if (current == it.value())
            continue;
        const int fd = ::open(QFile::encodeName(it.key()).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) { close(); return systemReason(errno); }
        struct stat held {};
        const State expected = step.guards.value(it.key());
        if (::fstat(fd, &held) != 0 || held.st_dev != st.st_dev || held.st_ino != st.st_ino
            || quint64(held.st_dev) != expected.device || quint64(held.st_ino) != expected.inode) {
            ::close(fd); close(); return QStringLiteral("the file changed while Undo was running");
        }
        values.append({fd, it.value(), current});
    }
    for (const Value &value : values) {
        if (!writeTagsValue(value.fd, value.before, &error)) { rollback(); close(); return error; }
    }
    for (const Record &row : rowsIn(store, step.scope))
        store.remove(row.path);
    for (const Record &row : step.beforeRows)
        store.put(row);
    if (!store.commit(&error)) { rollback(); close(); return error; }
    close();
    if (!step.info.isEmpty() && !QFile::remove(step.info))
        return QStringLiteral("the file and tags were restored, but its Trash metadata could not be removed");
    return {};
}

UndoJournal::Step UndoJournal::remaining(const Step &step)
{
    if (step.kind == QLatin1String("tag") || step.kind == QLatin1String("rows"))
        return step;
    Step result = step;
    result.guards.clear();
    QString error;
    if (step.kind == QLatin1String("mv") || step.kind == QLatin1String("trash")) {
        result.kind = QStringLiteral("tag");
        result.to = step.from;
        result.from.clear();
        result.guards.insert(result.to, fingerprint(result.to, &error));
    } else if (step.kind == QLatin1String("cp") || step.kind == QLatin1String("mkdir")
               || step.kind == QLatin1String("created")) {
        result.kind = QStringLiteral("rows");
    } else {
        result.guards = step.guards;
    }
    Store store(defaultIndexPath());
    if (store.load(&error))
        result.afterRows = rowsIn(store, result.scope);
    return result;
}

void UndoJournal::undo(VerbReceipt done)
{
    if (m_busy) {
        done(false, {}, QStringLiteral("An operation is still running; wait before Undo"));
        return;
    }
    if (m_entries.isEmpty()) {
        done(false, {}, QStringLiteral("Nothing to undo in this window session"));
        return;
    }
    if (!m_entries.last().refusal.isEmpty()) {
        done(false, {}, QStringLiteral("Cannot undo: %1").arg(m_entries.last().refusal));
        return;
    }
    setBusy(true);
    auto next = std::make_shared<std::function<void()>>();
    QPointer<UndoJournal> self(this);
    *next = [self, next, done] {
        if (!self) { *next = nullptr; return; }
        if (self->m_entries.last().steps.isEmpty()) {
            const QString label = self->m_entries.takeLast().label;
            self->setBusy(false);
            done(true, {}, QStringLiteral("Undid %1").arg(label));
            *next = nullptr;
            return;
        }
        const Step step = self->m_entries.last().steps.last();
        background<QString>(self, [step] { return check(step); }, [self, next, done, step](const QString &refusal) {
            if (!self) { *next = nullptr; return; }
            const auto finish = [self, next, done, step](bool ok, const QString &, const QString &error) {
                if (!self) { *next = nullptr; return; }
                if (!ok) {
                    self->setBusy(false);
                    done(false, {}, QStringLiteral("Cannot undo: %1").arg(error));
                    *next = nullptr;
                    return;
                }
                if (step.temporaryBackup) {
                    self->m_saved.erase(std::remove_if(self->m_saved.begin(), self->m_saved.end(), [&](const Step &saved) {
                        return step.scope.contains(saved.to);
                    }), self->m_saved.end());
                }
                self->m_entries.last().steps.removeLast();
                if (step.kind != QLatin1String("mv") && step.kind != QLatin1String("trash")) {
                    (*next)();
                    return;
                }
                background<State>(self, [step] {
                    QString error;
                    return fingerprint(step.from, &error);
                }, [self, next, step](const State &restored) {
                    for (Entry &entry : self->m_entries) {
                        for (Step &older : entry.steps) {
                            auto it = older.guards.find(step.from);
                            if (it != older.guards.end() && it->device == step.original.device
                                && it->inode == step.original.inode && it->birth == step.original.birth
                                && it->digest == restored.digest) {
                                it->device = restored.device;
                                it->inode = restored.inode;
                                it->birth = restored.birth;
                            }
                        }
                    }
                    (*next)();
                });
            };
            if (!refusal.isEmpty()) { finish(false, {}, refusal); return; }
            const auto restored = [self, step, finish](bool ok, const QString &, const QString &error) {
                if (!self)
                    return;
                if (!ok) { finish(false, {}, error); return; }
                background<QString>(self, [step] {
                    if (step.kind == QLatin1String("created")) {
                        QString leaf;
                        const int parent = openDirectoryNoFollow(typedDirectory(step.to, &leaf));
                        if (parent < 0) return systemReason(errno);
                        const QByteArray name = QFile::encodeName(leaf);
                        const State expected = step.guards.value(step.to);
                        struct stat current {};
                        QString error;
                        if (::fstatat(parent, name.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0)
                            error = systemReason(errno);
                        else if (quint64(current.st_dev) != expected.device || quint64(current.st_ino) != expected.inode)
                            error = QStringLiteral("the target changed while Undo was running");
                        else
                            error = removeTreeAt(parent, name);
                        ::close(parent);
                        return error;
                    }
                    if (step.kind == QLatin1String("mkdir")
                        && ::rmdir(QFile::encodeName(step.to).constData()) != 0)
                        return QStringLiteral("cannot remove %1: %2").arg(spelled(step.to), systemReason(errno));
                    return QString();
                }, [self, step, finish](const QString &error) {
                    if (!error.isEmpty()) { finish(false, {}, error); return; }
                    background<Step>(self, [step] { return remaining(step); }, [self, finish](const Step &pending) {
                        self->m_entries.last().steps.last() = pending;
                        background<QString>(self, [pending] { return restoreTags(pending); },
                            [finish](const QString &error) { finish(error.isEmpty(), {}, error); });
                    });
                });
            };
            const QStringList args = inverse(step);
            if (args.isEmpty())
                restored(true, {}, {});
            else
                runVerb(args, restored);
        });
    };
    (*next)();
}

} // namespace omafiles
