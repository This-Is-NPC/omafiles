#pragma once

#include "Record.h"

#include <QMap>
#include <QString>
#include <QVector>

#include <memory>

class QLockFile;

namespace omafiles {

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

    /// Reads the index without taking the lock. For a verb that only answers
    /// questions: it cannot see a half-written file, because a commit lands by
    /// rename.
    bool load(QString *error);

    /// Takes the lock and then reads. For a verb that is going to write: the
    /// lock has to be older than the copy in memory, or the copy is stale
    /// before it is modified.
    bool lockAndLoad(QString *error);

    /// Writes and releases. Refuses if the lock is not held, so a caller that
    /// loaded read-only cannot commit by accident.
    bool commit(QString *error);

    bool holdsLock() const;

    /// How long `lockAndLoad` waits for another writer. Five seconds suits a
    /// command somebody typed; a caller that must not stall can say so.
    void setLockTimeout(int milliseconds) { m_lockTimeout = milliseconds; }

    Record get(const QString &path) const;
    bool contains(const QString &path) const;
    void put(Record record);
    void remove(const QString &path);

    QVector<Record> records() const;

private:
    QString m_path;
    QMap<QString, Record> m_rows;
    std::unique_ptr<QLockFile> m_lock;
    int m_lockTimeout = 5000;
};

} // namespace omafiles
