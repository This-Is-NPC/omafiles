#pragma once

#include "Verb.h"
#include "Record.h"

#include <QObject>
#include <QMap>
#include <QSet>
#include <QVector>

namespace omafiles {

class UndoJournal : public QObject
{
    Q_OBJECT
public:
    explicit UndoJournal(QObject *parent = nullptr);
    ~UndoJournal() override;
    bool waitingForVerb() const { return m_waitingForVerb; }
    bool busy() const { return m_busy; }
    bool available() const { return !m_entries.isEmpty() && !m_busy; }
    QString label() const;
    void run(const QVector<QStringList> &commands, const QString &label, VerbReceipt done, bool askConflicts = false);
    void install(const QStringList &paths, const QString &directory, const QString &label, VerbReceipt done,
                 std::function<bool()> cancelled = {}, std::function<void(int, int)> progress = {});
    void resolveConflict(const QString &policy, bool all);
    void folderCreated(const QString &path);
    void undo(VerbReceipt done);

signals:
    void changed();
    void conflictRequested(const QString &from, const QString &to);

private:
    struct State {
        QByteArray digest;
        quint64 device = 0;
        quint64 inode = 0;
        QByteArray birth;
    };
    struct Tags {
        Record row;
        QByteArray value;
    };
    struct Step {
        QString kind;
        bool temporaryBackup = false;
        QString from;
        QString to;
        QString info;
        State original;
        QMap<QString, State> guards;
        QByteArray infoDigest;
        QMap<QString, Record> beforeRows;
        QMap<QString, Record> afterRows;
        QMap<QString, QByteArray> beforeValues;
        QStringList scope;
    };
    struct Entry {
        QString label;
        QVector<Step> steps;
        QString refusal;
    };
    struct Before {
        QMap<QString, Record> rows;
        QMap<QString, Tags> tags;
        QMap<QString, State> files;
        QSet<QString> trashSeen;
        QString error;
    };
    static State fingerprint(const QString &path, QString *error, bool emptyFolder = false);
    static Before capture(const QStringList &arguments);
    static QVector<Step> completed(const QStringList &arguments, const QString &output,
                                   const Before &captured, QString *error);
    static QString check(const Step &step);
    static QString restoreTags(const Step &step);
    static Step remaining(const Step &step);
    static QStringList inverse(const Step &step);
    static void discard(const QVector<Step> &steps);
    void run(const QVector<QStringList> &commands, const QString &label, VerbReceipt done, bool askConflicts,
             const QStringList &installedSources, std::function<bool()> cancelled,
             std::function<void(int, int)> progress);
    void append(Entry entry);
    void setBusy(bool busy);

    std::function<void(const QString &, bool)> m_conflict;
    bool m_waitingForVerb = false;
    bool m_busy = false;
    QVector<Entry> m_entries;
    QVector<Step> m_saved;
};

} // namespace omafiles
