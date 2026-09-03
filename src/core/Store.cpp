#include "Store.h"

#include "Tag.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QTextStream>

namespace omafiles {

Store::Store(QString indexPath)
    : m_path(std::move(indexPath))
{
}

Store::~Store() = default;

bool Store::holdsLock() const
{
    return m_lock && m_lock->isLocked();
}

bool Store::lockAndLoad(QString *error)
{
    if (!holdsLock()) {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        m_lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
        if (!m_lock->tryLock(m_lockTimeout)) {
            m_lock.reset();
            if (error)
                *error = QStringLiteral("store: cannot lock %1").arg(m_path);
            return false;
        }
    }
    return load(error);
}

bool Store::load(QString *error)
{
    m_rows.clear();
    QFile file(m_path);
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("store: cannot read %1").arg(m_path);
        return false;
    }
    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral("store: malformed jsonl at %1:%2")
                             .arg(m_path)
                             .arg(lineNumber);
            return false;
        }
        const Record record = Record::fromJson(doc.object(), error);
        if (record.path.isEmpty())
            return false;
        if (!record.tags.isEmpty())
            m_rows.insert(record.path, record);
    }
    return true;
}

bool Store::commit(QString *error)
{
    if (!holdsLock()) {
        if (error)
            *error = QStringLiteral("store: refusing to write %1 without the lock; "
                                    "load it with lockAndLoad to change it")
                         .arg(m_path);
        return false;
    }
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("store: cannot write %1").arg(m_path);
        return false;
    }
    QTextStream out(&file);
    for (const Record &record : m_rows) {
        const QJsonDocument doc(record.toJson());
        out << QString::fromUtf8(doc.toJson(QJsonDocument::Compact)) << '\n';
    }
    out.flush();
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("store: cannot commit %1").arg(m_path);
        return false;
    }
    // Held from before the read to after the rename, so no other writer saw the
    // copy this one started from.
    m_lock.reset();
    return true;
}

Record Store::get(const QString &path) const
{
    return m_rows.value(path);
}

bool Store::contains(const QString &path) const
{
    return m_rows.contains(path);
}

void Store::put(Record record)
{
    record.tags = uniqueSorted(record.tags);
    if (record.tags.isEmpty()) {
        m_rows.remove(record.path);
        return;
    }
    m_rows.insert(record.path, record);
}

void Store::remove(const QString &path)
{
    m_rows.remove(path);
}

QVector<Record> Store::records() const
{
    return m_rows.values();
}

} // namespace omafiles
