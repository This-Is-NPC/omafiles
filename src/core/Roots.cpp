#include "Roots.h"

#include "Paths.h"
#include "Store.h"
#include "Tag.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>

#include <cstdlib>
#include <sys/stat.h>

#include <utility>

namespace omafiles {

namespace {

QMutex &tableMutex()
{
    static QMutex mutex;
    return mutex;
}

QVector<Root> &table()
{
    static QVector<Root> declared;
    return declared;
}

bool &readRespelled()
{
    static bool respelled = false;
    return respelled;
}

// A mount point as `/proc/self/mountinfo` writes it: a space, tab, newline or
// backslash in it is `\` and three octal digits.
QByteArray mountTarget(const QByteArray &field)
{
    QByteArray target;
    for (qsizetype i = 0; i < field.size(); ++i) {
        const auto octal = [&field](qsizetype at) {
            return at < field.size() && field.at(at) >= '0' && field.at(at) <= '7';
        };
        if (field.at(i) == '\\' && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            target += char(((field.at(i + 1) - '0') << 6) | ((field.at(i + 2) - '0') << 3)
                           | (field.at(i + 3) - '0'));
            i += 3;
        } else {
            target += field.at(i);
        }
    }
    return target;
}

// Whether something is mounted at `path`, the way util-linux `mountpoint(1)`
// asks: as a mount point in the kernel's table, which holds a bind mount from
// the same filesystem too. Where the table will not read, by the fallback it
// uses: a mount point is on a device the directory holding it is not.
//
// Present for a mount, Unreachable for none, Unknown where either `stat` fails
// for any reason but ENOENT.
PathState mountState(const QString &path, QString *error)
{
    struct stat here {};
    PathState found = statState(path, error, true, &here);
    if (found != PathState::Present)
        return found == PathState::Missing ? PathState::Unreachable : found;
    QFile table(QStringLiteral("/proc/self/mountinfo"));
    if (openToRead(&table, {}, nullptr, Reading::Stream) == PathState::Present) {
        // The table holds resolved paths, as `mountpoint` compares them, and
        // `rootState` asks only of a root that resolves to itself.
        const QByteArray wanted = QFile::encodeName(path);
        for (const QByteArray &line : table.readAll().split('\n')) {
            const QList<QByteArray> fields = line.split(' ');
            if (fields.size() > 4 && mountTarget(fields.at(4)) == wanted)
                return PathState::Present;
        }
        return PathState::Unreachable;
    }
    const QString parent = typedDirectory(path, nullptr);
    struct stat above {};
    found = statState(parent, error, true, &above);
    if (found != PathState::Present)
        return found == PathState::Missing ? PathState::Unreachable : found;
    return parent == path || here.st_dev != above.st_dev ? PathState::Present
                                                         : PathState::Unreachable;
}

} // namespace

QString normalizeRootId(const QString &raw, QString *error)
{
    const QString folded = asciiFolded(raw);
    bool shaped = !folded.isEmpty() && folded.size() <= 64;
    for (int i = 0; shaped && i < folded.size(); ++i) {
        const QChar c = folded.at(i);
        const bool alnum = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
        // The first character carries the whole rule about `.`: an id that may
        // not begin with one cannot be `.` or `..`, which are the two names a
        // directory of caches must never be handed.
        shaped = alnum || (i > 0
                           && (c == QLatin1Char('-') || c == QLatin1Char('_')
                               || c == QLatin1Char('.')));
    }
    if (!shaped) {
        if (error)
            *error = QStringLiteral("%1 is not an id; letters, digits, `-`, `_` "
                                    "and `.`, starting with a letter or a digit, at "
                                    "most 64").arg(spelled(raw));
        return {};
    }
    return folded;
}

QString normalizeRootCheck(const QString &raw, QString *error)
{
    if (isCleanRelative(raw) && raw != QLatin1String("."))
        return raw;
    if (error)
        *error = QStringLiteral("--check takes a path inside the root, relative to it, not %1").arg(spelled(raw));
    return {};
}

QString defaultRootsPath()
{
    return QDir(configHome()).filePath(QStringLiteral("omafiles/roots.jsonl"));
}

QVector<Root> roots()
{
    const QMutexLocker lock(&tableMutex());
    return table();
}

QVector<Root> setRoots(QVector<Root> declared)
{
    const QMutexLocker lock(&tableMutex());
    QVector<Root> previous = table();
    table() = std::move(declared);
    return previous;
}

bool loadRoots(const QString &path, QString *error)
{
    QVector<Root> declared;
    bool respelled = false;
    QVector<KeptLine> lines;
    QString why;
    if (!readKeptLines(path, QStringLiteral("roots"), &lines, &why)) {
        if (error)
            *error = why;
        return false;
    }
    for (const KeptLine &line : std::as_const(lines)) {
        const QJsonObject &object = line.object;
        const QJsonValue id = object.value(QStringLiteral("id"));
        const QJsonValue where = object.value(QStringLiteral("path"));
        const QJsonValue mount = object.value(QStringLiteral("mount"));
        const QJsonValue check = object.value(QStringLiteral("check"));
        Root root{id.toString(), where.toString(), mount.toBool(), check.toString()};
        // The shape `roots add` writes, refused rather than cleaned, because a
        // row and a cache file are placed by it: an id `../x` names a cache
        // outside the cache directory, and a relative path moves with the cwd.
        const auto problem = [&]() -> QString {
            if (!id.isString() || !where.isString() || root.id.isEmpty() || root.path.isEmpty())
                return QStringLiteral("it needs an id and a path");
            if (!mount.isUndefined() && mount != QJsonValue(true))
                return QStringLiteral("its mount is not true");
            if (!check.isUndefined() && (!check.isString() || root.check.isEmpty()))
                return QStringLiteral("its check is not a path");
            if (!isCleanAbsolute(root.path))
                return QStringLiteral("%1 is not an absolute path in its clean form")
                    .arg(spelled(root.path));
            if (normalizeRootId(root.id, nullptr) != root.id)
                return QStringLiteral("%1 is not an id in the form `roots add` writes")
                    .arg(spelled(root.id));
            if (!root.check.isEmpty() && normalizeRootCheck(root.check, nullptr).isEmpty())
                return QStringLiteral("%1 is not a clean path inside the root")
                    .arg(spelled(root.check));
            // Where its directory stores the name now, as a row is placed.
            if (const QString stored = storedSpelling(root.path); stored != root.path) {
                root.path = stored;
                respelled = true;
            }
            for (const Root &already : std::as_const(declared)) {
                if (already.id == root.id)
                    return QStringLiteral("%1 is declared twice").arg(root.id);
                if (already.path == root.path)
                    return QStringLiteral("%1 is declared as %2 and as %3")
                        .arg(spelled(root.path), already.id, root.id);
            }
            return {};
        };
        why = problem();
        if (!why.isEmpty()) {
            if (error)
                *error = QStringLiteral("roots: malformed jsonl at %1:%2: %3")
                             .arg(spelled(path), QString::number(line.number), why);
            return false;
        }
        declared.append(root);
    }
    {
        const QMutexLocker lock(&tableMutex());
        table() = std::move(declared);
        readRespelled() = respelled;
    }
    return true;
}

bool loadRoots(QString *error)
{
    return loadRoots(defaultRootsPath(), error);
}

bool rootsRespelled()
{
    const QMutexLocker lock(&tableMutex());
    return readRespelled();
}

bool saveRoots(const QString &path, const QVector<Root> &declared, QString *error)
{
    QVector<QJsonObject> lines;
    for (const Root &root : declared) {
        QJsonObject object{
            {QStringLiteral("id"), root.id},
            {QStringLiteral("path"), root.path},
        };
        if (root.mount)
            object.insert(QStringLiteral("mount"), true);
        if (!root.check.isEmpty())
            object.insert(QStringLiteral("check"), root.check);
        lines.append(object);
    }
    QString why;
    if (!writeKeptLines(path, lines, &why)) {
        if (error)
            *error = QStringLiteral("roots: cannot write %1: %2").arg(spelled(path), why);
        return false;
    }
    return true;
}

std::optional<Root> rootById(const QString &id)
{
    for (const Root &root : roots()) {
        if (root.id == id)
            return root;
    }
    return std::nullopt;
}

std::optional<Root> rootFor(const QString &absolutePath)
{
    std::optional<Root> best;
    for (const Root &root : roots()) {
        if (!isUnderDirectory(absolutePath, root.path) && absolutePath != root.path)
            continue;
        // The longest match, so a root declared inside another one takes the
        // rows under it rather than losing them to the one above.
        if (!best || root.path.size() > best->path.size())
            best = root;
    }
    return best;
}

std::optional<Root> rootAt(const QString &absolutePath)
{
    const auto root = rootFor(absolutePath);
    return root && root->path == absolutePath ? root : std::nullopt;
}

// Three answers, weakest last.
//
// A declared `check` is the sure one: it is a name that only exists when the
// storage does. Without it, a mount root is here when something is mounted
// where it says. And a root that is simply part of the filesystem is here when
// its directory is -- which is the same test as before roots existed, so
// declaring one over a folder that is always present changes nothing.
PathState rootState(const Root &root, QString *error)
{
    // A root is a resolved path: one a symlink now stands at or above leads to
    // some other directory, so the storage declared there is away.
    const QByteArray declared = QFile::encodeName(root.path);
    if (char *real = ::realpath(declared.constData(), nullptr)) {
        const bool moved = declared != real;
        std::free(real);
        if (moved)
            return PathState::Unreachable;
    }
    if (root.mount && root.check.isEmpty())
        return mountState(root.path, error);
    const QString probe = root.check.isEmpty() ? root.path : QDir(root.path).filePath(root.check);
    const PathState found = statState(probe, error);
    return found == PathState::Missing ? PathState::Unreachable : found;
}

QString unreachableReason(const QString &path, const Root &root)
{
    return QStringLiteral("%1: root %2 is declared and not here").arg(spelled(path), root.id);
}

void splitByRoot(const QString &absolutePath, QString *rootId, QString *relative)
{
    const auto root = rootFor(absolutePath);
    if (!root) {
        if (rootId)
            rootId->clear();
        if (relative)
            relative->clear();
        return;
    }
    if (rootId)
        *rootId = root->id;
    // `.` for the root itself, so that the pair always spells something and an
    // empty relative can go on meaning "no root".
    if (relative)
        *relative = relativeTo(absolutePath, root->path);
}

QString joinRoot(const QString &rootId, const QString &relative)
{
    const auto root = rootById(rootId);
    if (!root)
        return {};
    if (relative == QLatin1String("."))
        return root->path;
    return QDir(root->path).filePath(relative);
}

} // namespace omafiles
