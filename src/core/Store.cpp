#include "Store.h"

#include "Paths.h"
#include "Roots.h"
#include "Tag.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QRandomGenerator>
#include <QSet>

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omafiles {

// Why an existing `path` may not be written, as `QSaveFile` refuses one although
// a rename would replace it; empty where it may or is not there.
static QString replaceRefusal(const QByteArray &path)
{
    if (::faccessat(AT_FDCWD, path.constData(), W_OK, AT_EACCESS) == 0 || errno == ENOENT)
        return {};
    return systemReason(errno);
}

QString writeRefusal(const QString &path)
{
    if (QFileInfo(path).isDir())
        return systemReason(EISDIR);
    QByteArray probe = QFile::encodeName(path) + ".XXXXXX";
    const int fd = ::mkstemp(probe.data());
    if (fd < 0)
        return systemReason(errno);
    const int failure = ::write(fd, "\n", 1) == 1 ? 0 : errno;
    ::close(fd);
    ::unlink(probe.constData());
    return failure == 0 ? QString() : systemReason(failure);
}

bool writeKeptLines(const QString &path, const QVector<QJsonObject> &objects, QString *why)
{
    QByteArray body;
    for (const QJsonObject &object : objects)
        body += QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    return writeKeptFile(path, body, why);
}

// A file beside `path` nobody holds, opened to write with `mode` under the
// umask, as `QTemporaryFileEngine` makes one; -1 with errno otherwise.
static int createBeside(const QByteArray &path, mode_t mode, QByteArray *temp)
{
    int fd = -1;
    do {
        *temp = path + '.' + QByteArray::number(QRandomGenerator::global()->generate64(), 36);
        fd = ::open(temp->constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EEXIST);
    return fd;
}

// `QSaveFile`'s sequence -- through a symlink to its file, a new file beside it
// given the old one's mode, fsync, rename -- in POSIX calls, so a failure keeps
// the errno of the call that failed, as git's lockfile does.
bool writeKeptFile(const QString &path, const QByteArray &body, QString *why)
{
    if (!makePath(typedDirectory(path, nullptr), why))
        return false;
    QByteArray target = QFile::encodeName(path);
    struct stat st;
    const bool exists = ::lstat(target.constData(), &st) == 0;
    if (exists && S_ISLNK(st.st_mode)) {
        char *real = ::realpath(target.constData(), nullptr);
        if (!real || ::stat(real, &st) != 0) {
            *why = systemReason(errno);
            std::free(real);
            return false;
        }
        target = real;
        std::free(real);
    }
    if (*why = replaceRefusal(target); !why->isEmpty())
        return false;
    QByteArray temp;
    const int fd = createBeside(target, exists ? 0600 : 0666, &temp);
    if (fd < 0) {
        *why = systemReason(errno);
        return false;
    }
    int failure = 0;
    for (qsizetype done = 0; failure == 0 && done < body.size();) {
        const ssize_t wrote = ::write(fd, body.constData() + done, size_t(body.size() - done));
        if (wrote >= 0)
            done += wrote;
        else if (errno != EINTR)
            failure = errno;
    }
    if (failure == 0 && exists && ::fchmod(fd, st.st_mode & 07777) != 0)
        failure = errno;
    if (failure == 0 && ::fsync(fd) != 0)
        failure = errno;
    if (::close(fd) != 0 && failure == 0)
        failure = errno;
    if (failure == 0 && ::rename(temp.constData(), target.constData()) != 0)
        failure = errno;
    if (failure == 0)
        return true;
    ::unlink(temp.constData());
    *why = systemReason(failure);
    return false;
}

QJsonDocument jsonDocument(QByteArray bytes, QJsonParseError *error)
{
    // Qt decodes a string holding no escape with `QString::fromUtf8`, which
    // drops a U+FEFF that begins it; spelled as an escape it is kept.
    bytes.replace("\"\xef\xbb\xbf", "\"\\ufeff");
    return QJsonDocument::fromJson(bytes, error);
}

bool keptObject(const QByteArray &bytes, QJsonObject *object, QString *why)
{
    if (bytes.startsWith("\xef\xbb\xbf")) {
        *why = QStringLiteral("it begins with a byte order mark, which this program never writes");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = jsonDocument(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *why = QString();
        return false;
    }
    *object = doc.object();
    return true;
}

bool readKeptLines(const QString &path, const QString &owner, QVector<KeptLine> *lines,
                   QString *error)
{
    lines->clear();
    QFile file(path);
    QString why;
    const PathState opened = openToRead(&file, QIODevice::Text, &why);
    if (opened == PathState::Missing)
        return true;
    if (opened != PathState::Present) {
        *error = QStringLiteral("%1: cannot read %2").arg(owner, why);
        return false;
    }
    for (int number = 1; !file.atEnd(); ++number) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        KeptLine kept{number, {}};
        if (!keptObject(line, &kept.object, &why)) {
            *error = QStringLiteral("%1: malformed jsonl at %2:%3")
                         .arg(owner, spelled(path))
                         .arg(number);
            if (!why.isEmpty())
                *error += QStringLiteral(": ") + why;
            return false;
        }
        lines->append(kept);
    }
    return true;
}

QString keptTagsProblem(const QJsonValue &tags)
{
    if (!tags.isArray() || tags.toArray().isEmpty())
        return QStringLiteral("it has no tags, and a row with none is one no writer keeps");
    QStringList written;
    for (const QJsonValue &tag : tags.toArray()) {
        if (!tag.isString())
            return QStringLiteral("a tag is not a string");
        if (!isValidTag(tag.toString()))
            return QStringLiteral("%1 is not a tag").arg(spelled(tag.toString()));
        written.append(tag.toString());
    }
    if (uniqueSorted(written) != written)
        return QStringLiteral("its tags are not sorted and each once, the way they are written: %1")
            .arg(written.join(QLatin1Char(',')));
    return {};
}

Record keptRow(const QJsonObject &object, QString *why)
{
    const Record record = Record::fromJson(object, why);
    if (record.path.isEmpty())
        return {};
    *why = keptTagsProblem(object.value(QStringLiteral("tags")));
    return why->isEmpty() ? record : Record();
}

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
    m_contended = false;
    if (!holdsLock()) {
        QString why;
        if (!makePath(typedDirectory(m_path, nullptr), &why)) {
            if (error)
                *error = QStringLiteral("store: cannot lock %1: %2").arg(spelled(m_path), why);
            return false;
        }
        const QString lockFile = Store::lockPath(m_path);
        const QByteArray lockBytes = QFile::encodeName(lockFile);
        struct stat before {};
        const bool there = ::lstat(lockBytes.constData(), &before) == 0;
        m_lock = std::make_unique<QLockFile>(lockFile);
        if (!m_lock->tryLock(m_lockTimeout)) {
            // A foreign lock names no pid and is the very file there when the wait
            // began; one gone or replaced since changed hands, maybe to a writer mid-write.
            qint64 pid = 0;
            QString host;
            QString application;
            const bool held = m_lock->error() == QLockFile::LockFailedError;
            struct stat after {};
            const bool foreign = held && !m_lock->getLockInfo(&pid, &host, &application)
                && there && ::lstat(lockBytes.constData(), &after) == 0
                && after.st_dev == before.st_dev && after.st_ino == before.st_ino
                && after.st_ctim.tv_sec == before.st_ctim.tv_sec
                && after.st_ctim.tv_nsec == before.st_ctim.tv_nsec;
            m_contended = held && !foreign;
            QString reason = m_contended
                ? QStringLiteral("another writer holds it")
                : held ? QStringLiteral("a lock file is there that no omafiles wrote: %1")
                             .arg(spelled(lockFile))
                       : writeRefusal(lockFile);
            if (reason.isEmpty())
                reason = QStringLiteral("the system would not create %1").arg(spelled(lockFile));
            if (error)
                *error = QStringLiteral("store: cannot lock %1: %2").arg(spelled(m_path), reason);
            m_lock.reset();
            return false;
        }
        // An index the commit would refuse is found out before a verb acts on
        // a file, not after.
        if (const QString refused = replaceRefusal(QFile::encodeName(m_path));
            !refused.isEmpty()) {
            if (error)
                *error = QStringLiteral("store: cannot write %1: %2").arg(spelled(m_path), refused);
            m_lock.reset();
            return false;
        }
    }
    return load(error);
}

bool Store::load(QString *error)
{
    m_rows.clear();
    m_read.clear();
    QVector<KeptLine> lines;
    QString why;
    if (!readKeptLines(m_path, QStringLiteral("store"), &lines, &why)) {
        if (error)
            *error = why;
        return false;
    }
    Spellings known;
    QSet<QString> respelled;      // lines in a spelling their directory no longer stores
    QSet<QString> followedOnly;   // rows only such lines reached
    m_rewrite = false;
    for (const KeptLine &line : std::as_const(lines)) {
        m_read.append(line.object);
        Record record = keptRow(line.object, &why);
        if (record.path.isEmpty()) {
            if (error)
                *error = QStringLiteral("store: malformed jsonl at %1:%2: %3")
                             .arg(spelled(m_path), QString::number(line.number), why);
            return false;
        }
        // A name a folding directory now stores in another spelling: the
        // row follows the file, and one already there takes its tags.
        const QString given = record.path;
        record.path = storedSpelling(given, &known);
        const bool moved = record.path != given;
        const auto held = m_rows.lowerBound(record.path);
        const bool meets = held != m_rows.end() && held.key() == record.path;
        // Two lines for one path is two answers to one question, and no rule
        // here says which of them is the row.
        if (moved ? respelled.contains(given) : meets && !followedOnly.contains(given)) {
            if (error)
                *error = QStringLiteral("store: malformed jsonl at %1:%2: %3 is on an "
                                        "earlier line too")
                             .arg(spelled(m_path), QString::number(line.number),
                                  spelled(given));
            return false;
        }
        if (moved) {
            respelled.insert(given);
            if (!record.root.isEmpty())
                record.relative = relativeTo(record.path, rootById(record.root)->path);
            m_rewrite = true;
        }
        if (meets) {
            if (!moved)
                followedOnly.remove(given);
            held->tags = uniqueSorted(held->tags + record.tags);
            held->tagged = qMax(held->tagged, record.tagged);
        } else {
            if (moved)
                followedOnly.insert(record.path);
            m_rows.insert(held, record.path, record);
        }
    }
    m_loaded = m_rows;
    return true;
}

bool Store::commit(QString *error)
{
    if (!holdsLock()) {
        if (error)
            *error = QStringLiteral("store: refusing to write %1 without the lock; "
                                    "load it with lockAndLoad to change it")
                         .arg(spelled(m_path));
        return false;
    }
    QVector<QJsonObject> rows;
    for (const Record &record : m_rows)
        rows.append(record.toStoredJson());
    QString why;
    // A rewrite is of rows whose stored form changed, so one that changes no
    // line writes nothing, and makes no index where none was.
    if ((m_rows != m_loaded || (m_rewrite && rows != m_read))
        && !writeKeptLines(m_path, rows, &why)) {
        if (error)
            *error = QStringLiteral("store: cannot write %1: %2").arg(spelled(m_path), why);
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
