#pragma once

#include "Record.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QString>
#include <QVector>

#include <memory>

class QLockFile;

namespace omafiles {

/// `QJsonDocument::fromJson`, keeping a U+FEFF that begins a string, which Qt
/// drops and a name may begin with.
QJsonDocument jsonDocument(QByteArray bytes, QJsonParseError *error);

/// One JSON object as this program writes one. Qt's parser skips a leading byte
/// order mark, which is refused here; a duplicated key keeps its last value, as
/// jq, Python and `JSON.parse` do and RFC 8259 allows.
bool keptObject(const QByteArray &bytes, QJsonObject *object, QString *why);

struct KeptLine {
    int number = 0;
    QJsonObject object;
};

/// Every line of a kept jsonl file -- the index, the roots, the views -- past
/// the blank ones, each through `keptObject`. A file that is not there is no
/// lines. `owner` begins each error: `store: malformed jsonl at <path>:<n>`.
bool readKeptLines(const QString &path, const QString &owner, QVector<KeptLine> *lines,
                   QString *error);

/// `body` as the kept file `path`, by rename into a directory made first. False
/// with the system's reason, which names the directory where that is what would
/// not be made; a file there that may not be written is refused, not replaced.
bool writeKeptFile(const QString &path, const QByteArray &body, QString *why);
/// `writeKeptFile` of `objects`, one compact JSON object per line.
bool writeKeptLines(const QString &path, const QVector<QJsonObject> &objects, QString *why);

/// Why the system will not let a file be made and written as `path`, learned by
/// making one beside it, since QLockFile keeps no errno; empty where it will.
QString writeRefusal(const QString &path);

/// Why a kept `tags` is not the list its writer writes -- present, not empty,
/// valid tags, sorted and each once -- or empty where it is.
QString keptTagsProblem(const QJsonValue &tags);

/// One object held to the shape of an index row -- `Record::fromJson` and
/// `keptTagsProblem` -- or an empty record with why not. The index loader and
/// `import` both ask this.
Record keptRow(const QJsonObject &object, QString *why);

// The jsonl index, in memory.
//
// A writer holds the lock across the whole read-modify-write, not just the
// write -- git's lockfile discipline for its own index: the lock blocks other
// writers, readers never block, and the atomic rename means a reader sees
// either the old file or the new one and never half of either. Locking only
// the write makes every concurrent tagging run a lost update.
class Store {
public:
    explicit Store(QString indexPath);
    ~Store();

    QString indexPath() const { return m_path; }
    /// The lock a writer holds beside the index at `index`.
    static QString lockPath(const QString &index) { return index + QStringLiteral(".lock"); }

    /// Reads the index without taking the lock. For a verb that only answers
    /// questions: it cannot see a half-written file, because a commit lands by
    /// rename.
    bool load(QString *error);

    /// Takes the lock and then reads. For a verb that is going to write: the
    /// lock has to be older than the copy in memory, or the copy is stale
    /// before it is modified. An index there that may not be written fails here,
    /// before the verb acts on any file.
    bool lockAndLoad(QString *error);

    /// Writes and releases. Refuses if the lock is not held, so a caller that
    /// loaded read-only cannot commit by accident. An index whose rows are the
    /// ones it read, spelled as it read them, is released unwritten, as git
    /// writes its index only when an entry changed.
    bool commit(QString *error);
    /// Makes the next commit write every row in the form the roots give it,
    /// though no row changed.
    void rewrite() { m_rewrite = true; }

    bool holdsLock() const;
    /// Whether the last `lockAndLoad` failed because another writer holds the
    /// lock, as against the system refusing it or the index not reading.
    bool contended() const { return m_contended; }

    /// How long `lockAndLoad` waits for another writer. Five seconds suits a
    /// command somebody typed; a caller that must not stall can say so.
    void setLockTimeout(int milliseconds) { m_lockTimeout = milliseconds; }

    Record get(const QString &path) const;
    bool contains(const QString &path) const;
    /// The row the last load read for `path`, whatever has been put since; path
    /// empty where it read none.
    Record loaded(const QString &path) const { return m_loaded.value(path); }
    void put(Record record);
    void remove(const QString &path);

    QVector<Record> records() const;

private:
    QString m_path;
    QMap<QString, Record> m_rows;
    QMap<QString, Record> m_loaded;
    bool m_rewrite = false;   // a row read in a spelling its directory no longer stores, or `rewrite`
    QVector<QJsonObject> m_read;   // the lines the last load read, as read
    std::unique_ptr<QLockFile> m_lock;
    int m_lockTimeout = 5000;
    bool m_contended = false;
};

} // namespace omafiles
