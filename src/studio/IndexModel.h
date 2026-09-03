#pragma once

#include "Store.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>
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
    int tagCount() const;
    QAbstractListModel *tagsModel() const;

    QVector<Record> records() const { return m_store.records(); }
    QStringList tagsForPath(const QString &path) const;

    bool canShare() const;

    Q_INVOKABLE void reload();
    Q_INVOKABLE void openPath(const QString &path);
    Q_INVOKABLE void sharePath(const QString &path);

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

private:
    Store m_store;
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
