#pragma once

#include "Store.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>
#include <QVariantList>
#include <QVector>

namespace omafiles {

class TagListModel;

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
    Q_PROPERTY(int fileCount READ fileCount NOTIFY changed)
    Q_PROPERTY(int tagCount READ tagCount NOTIFY changed)
    Q_PROPERTY(bool canShare READ canShare NOTIFY changed)
    Q_PROPERTY(bool canOpenTerminal READ canOpenTerminal NOTIFY changed)
    Q_PROPERTY(bool canEdit READ canEdit NOTIFY changed)
    Q_PROPERTY(bool canOpenWith READ canOpenWith NOTIFY changed)
    /// What the last command the window ran said about itself.
    ///
    /// Separate from `error`, which describes a condition -- rows that will
    /// still be uncounted tomorrow. This is a receipt for something somebody
    /// just chose, it outranks the condition on the one line there is, and it
    /// survives the reload the command triggers: an index that changed under
    /// the window is no reason to take away the sentence explaining why.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QAbstractListModel *tags READ tagsModel CONSTANT)

public:
    explicit IndexModel(QObject *parent = nullptr);

    QString error() const { return m_error; }
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
    /// Rows on a declared root that is not plugged in. Not missing and not
    /// undescribable: they come back when the disk does, and until then nothing
    /// here drops them.
    int unreachableRows() const { return m_unreachable; }
    int tagCount() const;
    QAbstractListModel *tagsModel() const;

    QVector<Record> records() const { return m_store.records(); }
    Q_INVOKABLE QStringList tagsForPath(const QString &path) const;

    bool canShare() const;
    bool canOpenTerminal() const;
    bool canEdit() const;
    bool canOpenWith() const;
    QString notice() const { return m_notice; }

    Q_INVOKABLE void reload();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void sharePath(const QString &path);
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
    Q_INVOKABLE void duplicate(const QString &path);
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
    void changed();
    void noticeChanged();

private:
    void setNotice(const QString &message);
    /// Runs one verb and turns its receipt into the line the status bar shows:
    /// `success` when it worked, whatever the command line said when it did
    /// not. A run that worked re-reads the index rather than waiting for the
    /// watcher, so the list has caught up by the time the message appears.
    void report(const QStringList &arguments, const QString &success);
    /// Runs a list of commands one after another and reports the tally.
    ///
    /// One after another and never at once: a writer holds the store lock from
    /// its read to its commit, and a second one that finds it taken waits five
    /// seconds and then refuses. Six copies fired together would be one copy
    /// and five `store: cannot lock`.
    void reportEach(const QVector<QStringList> &runs, const QString &verb);

    Store m_store;
    QString m_notice;
    QString m_error;
    int m_present = 0;
    int m_missing = 0;
    int m_unknown = 0;
    int m_unreachable = 0;
    QFileSystemWatcher m_watcher;
    /// Coalesces the watcher. One CLI write is four filesystem events -- the
    /// lock file appearing, the temporary appearing, the rename, the lock file
    /// going -- and the window wants the state after the write, once, not four
    /// re-reads of the jsonl.
    QTimer m_settle;
    TagListModel *m_tags = nullptr;
};

} // namespace omafiles
