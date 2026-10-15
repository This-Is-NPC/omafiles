#pragma once

#include "Store.h"
#include "ArchiveJob.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>
#include <QVariantList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

namespace omafiles {

class TagListModel;
class UndoJournal;

class IndexModel : public QObject
{
    Q_OBJECT
    // A QML singleton rather than a context property: a context property is
    // invisible to `qmllint`, so every use of it is an unqualified access the
    // linter can only warn about. A declared singleton is a type the tooling
    // and the engine agree on, and it is looked up once rather than searched
    // for in the context chain.
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(int fileCount READ fileCount NOTIFY changed)
    Q_PROPERTY(int tagCount READ tagCount NOTIFY changed)
    Q_PROPERTY(bool canShare READ canShare NOTIFY changed)
    Q_PROPERTY(bool canOpenTerminal READ canOpenTerminal NOTIFY changed)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY changed)
    Q_PROPERTY(bool canOpenWith READ canOpenWith NOTIFY changed)
    /// Whether the scripts folder holds anything to run. Read off the disk
    /// each time it is asked, and asked again on every reload, so F5 is how a
    /// script written while the window is open reaches the menu.
    Q_PROPERTY(bool hasScripts READ hasScripts NOTIFY changed)
    Q_PROPERTY(omafiles::ArchiveJob *archive READ archive CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool operationBusy READ operationBusy NOTIFY undoChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY undoChanged)
    Q_PROPERTY(bool tagEditing READ tagEditing NOTIFY tagEditingChanged)
    // Results expire when their context changes; current conditions take precedence.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QAbstractListModel *tags READ tagsModel CONSTANT)

public:
    explicit IndexModel(QObject *parent = nullptr);
    ~IndexModel() override;

    QString error() const { return m_error; }
    bool loading() const { return bool(m_presenceCancellation) || m_settle.isActive(); }
    /// Index rows the system will describe: the number `omafiles ls` and
    /// `omafiles tags` answer with, and the one the header shows.
    int fileCount() const { return m_present; }
    /// The rows that number left out. `missingRows` are the ones whose file is
    /// gone, which `prune` drops; `unknownRows` are the ones the system merely
    /// declines to describe, which `prune` must not touch. The CLI says a short
    /// answer is short by exiting 1; the window has no exit code, so it says it
    /// through `error`.
    int missingRows() const { return m_missing; }
    int unknownRows() const { return m_unknown; }
    int tagCount() const;
    QAbstractListModel *tagsModel() const;

    /// The index as it was last read.
    const Store &store() const { return m_store; }
    Q_INVOKABLE QStringList tagsForPath(const QString &path) const;

    bool canShare() const;
    bool canOpenTerminal() const;
    bool canEdit() const;
    bool canOpenWith() const;
    bool hasScripts() const;
    QString notice() const { return m_notice; }
    ArchiveJob *archive() const { return m_archive; }
    Q_INVOKABLE void compress(const QStringList &paths, const QString &destination, const QString &format);
    Q_INVOKABLE void extract(const QString &path, const QString &destination);
    Q_INVOKABLE void cancelArchive();
    bool canUndo() const;
    bool operationBusy() const;
    bool waitingForVerb() const;
    QString undoLabel() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void resolveConflict(const QString &policy, bool all);
    bool tagEditing() const { return m_tagEditing; }
    Q_INVOKABLE QStringList tagChoices(const QStringList &paths) const;

    /// The person's own scripts, as `name` and `path`, the list the `Run a
    /// script` menu is built from. See `userScripts`.
    Q_INVOKABLE QVariantList scripts() const;
    /// Runs one over `paths`, standing in `directory`. What it does with them
    /// is its own business; the receipt here is only that it started.
    Q_INVOKABLE void runScript(const QString &script, const QStringList &paths,
                               const QString &directory);

    Q_INVOKABLE void reload();
    Q_INVOKABLE void refreshStorage();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void sharePath(const QString &path);
    Q_INVOKABLE void makeDefaultFileManager();
    Q_INVOKABLE void openTerminalIn(const QString &directory);
    Q_INVOKABLE void editPath(const QString &path);

    /// The applications registered for what `path` is, as `file` and `name`,
    /// the default one first. The list the `Open with` menu is built from.
    Q_INVOKABLE QVariantList applicationsFor(const QString &path) const;
    Q_INVOKABLE void openWith(const QString &path, const QString &desktopFile);
    /// Says on the status bar that the desktop has nothing registered for this
    /// file. The answer to `Open with` where the answer is nobody -- said,
    /// rather than shown as a menu with no entries in it.
    Q_INVOKABLE void noApplicationFor(const QString &path);

    /// The command line the desktop would run to open `path`, as text.
    ///
    /// What `omafiles open --dry-run` prints, answered here rather than by
    /// running the binary because the answer has to come back as a value: it is
    /// being copied to the clipboard, not reported. It asks `xdg-mime` twice,
    /// which is why it is asked when the entry is chosen and not when it is
    /// drawn.
    Q_INVOKABLE QString openCommandFor(const QString &path) const;

    /// Drops the receipt, so the status bar goes back to describing the index.
    /// Called when a menu opens: the message answered the last question, and
    /// asking a new one is as good a signal as any that it has been read.
    Q_INVOKABLE void clearNotice();
    Q_INVOKABLE void changeNoticeContext(const QString &directory);
    void expireNoticeForDirectory(const QString &directory);
    Q_INVOKABLE void setNotice(const QString &message);

    /// The verbs, as the menu asks for them.
    ///
    /// Each one builds a command line and hands it to `runVerb`; none of them
    /// touches the store directly. The window is a caller of `omafiles`, not a
    /// second implementation of it -- so `mv` refuses to clobber here for the
    /// same reason and in the same words as it does at a prompt, and a rule
    /// changed in one place changes in both.
    ///
    /// They all take a list, because the menu acts on the marked set as
    /// readily as on one row, and every one of these verbs takes as many
    /// operands as it is given in a single store write.
    Q_INVOKABLE void tagAdd(const QStringList &paths, const QString &tag);
    Q_INVOKABLE void tagRemove(const QStringList &paths, const QString &tag);
    Q_INVOKABLE void tagRename(const QStringList &paths, const QString &from, const QString &to);
    Q_INVOKABLE void tagClear(const QStringList &paths);
    Q_INVOKABLE void tagCopy(const QString &from, const QStringList &paths);
    /// `reindex --from-xattr` over a tree, which is how tags written onto files
    /// somewhere else come back into the index. Scoped to a folder and not to
    /// the row under the cursor because that is the only operand the verb
    /// takes: it walks a tree, and hands a file back as a usage error.
    Q_INVOKABLE void adoptTagsIn(const QString &directory);
    Q_INVOKABLE void trash(const QStringList &paths);
    Q_INVOKABLE void copyInto(const QStringList &paths, const QString &directory);
    Q_INVOKABLE void moveInto(const QStringList &paths, const QString &directory);
    Q_INVOKABLE void linkInto(const QStringList &paths, const QString &directory);
    Q_INVOKABLE void duplicate(const QString &path);
    /// `mv` to a new name beside the old one. Refuses a destination that is
    /// there, in `mv`'s words, before `mv` gets to answer differently.
    Q_INVOKABLE void rename(const QString &path, const QString &to);
    /// `mv --pattern` over a set: each renamed in its own folder by the
    /// template, and the refusals -- a name taken, two landing on one --
    /// are the verb's.
    Q_INVOKABLE void renameSet(const QStringList &paths, const QString &pattern);
    /// `chmod <mode>` over the paths. Who may change a mode is the system's to
    /// say, so a refusal is the verb's and lands on the status bar.
    Q_INVOKABLE void changeMode(const QStringList &paths, const QString &mode);
    /// `mkdir`. Not a verb of the command line, because the command line has
    /// none: a folder with nothing in it has no row, and the system call is the
    /// whole of the job. Reported here all the same, since the receipt goes
    /// where every other receipt goes. True when the folder is there now.
    Q_INVOKABLE bool makeFolder(const QString &directory, const QString &name);
    /// `symlink(2)`, for the reason `makeFolder` gives: a link has no row of
    /// its own -- `setxattr` follows it, so its tags are the target's -- and
    /// there is nothing for a verb to do. True when the link is there now.
    Q_INVOKABLE bool makeLink(const QString &target, const QString &link);
    Q_INVOKABLE void reindexTree(const QString &directory);
    Q_INVOKABLE void pruneTree(const QString &directory);
    Q_INVOKABLE void verifyTree(const QString &directory);

    /// The tag on one row of the vocabulary list.
    ///
    /// The tag panel is walked by the keyboard, and a keyboard walks past the
    /// bottom of the window. Reading the tag off the delegate would only work
    /// for the rows that happen to be drawn, so the row a scrolled-out cursor
    /// sits on would toggle nothing.
    Q_INVOKABLE QString tagAt(int row) const;
    Q_INVOKABLE int tagRowCount() const;

signals:
    void loadingChanged();
    void changed();
    void storageChanged();
    void undoChanged();
    void conflictRequested(const QString &from, const QString &to);
    void noticeChanged();
    void tagEditingChanged();
    void tagEditFinished(bool ok, const QString &receipt);
    void defaultManagerFinished(bool ok, const QString &text);

private:
    std::function<void(const QString &)> noticeReceiver();
    quint64 m_noticeContext = 0;
    quint64 m_archiveContext = 0;
    QString m_noticeDirectory;
    QByteArray m_noticeDirectoryState;

    /// Runs one verb and turns its receipt into the line the status bar shows:
    /// `success` when it worked, whatever the command line said when it did
    /// not. A run that worked re-reads the index rather than waiting for the
    /// watcher, so the list has caught up by the time the message appears.
    void report(const QStringList &arguments, const QString &success, bool tagEdit = false);
    /// Runs a list of commands one after another and reports the tally.
    ///
    /// One after another and never at once: a writer holds the store lock from
    /// its read to its commit, and a second one that finds it taken waits five
    /// seconds and then refuses. Six copies fired together would be one copy
    /// and five `store: cannot lock`.
    void reportEach(const QVector<QStringList> &runs, const QString &verb);

    struct Presence;
    static Presence countPresence(const QMap<QString, Record> &records,
                                  const std::shared_ptr<std::atomic_bool> &cancelled = {});
    void publishPresence(const Presence &presence, bool clearReceipt);
    void watchSources();
    void loadSources();
    QString m_rootsPath;
    std::shared_ptr<std::atomic_bool> m_presenceCancellation;
    QString m_loadError;

    void publishArchive(const QStringList &paths, const QString &directory, const QString &label);
    void finishArchive(bool ok, const QString &message, const QString &stage);
    ArchiveJob *m_archive = nullptr;
    QStringList m_archiveTags;
    UndoJournal *m_undo = nullptr;
    bool m_tagEditing = false;
    bool m_settingDefault = false;
    Store m_store;
    QString m_notice;
    QString m_error;
    int m_present = 0;
    int m_missing = 0;
    int m_unknown = 0;
    QFileSystemWatcher m_watcher;
    /// Coalesces the watcher. One CLI write is four filesystem events -- the
    /// lock file appearing, the temporary appearing, the rename, the lock file
    /// going -- and the window wants the state after the write, once, not four
    /// re-reads of the jsonl.
    QTimer m_settle;
    TagListModel *m_tags = nullptr;
};

} // namespace omafiles
