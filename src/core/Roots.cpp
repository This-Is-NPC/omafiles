#include "Roots.h"

#include "Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

#include <sys/stat.h>

#include <utility>

namespace omafiles {

namespace {

QVector<Root> &table()
{
    static QVector<Root> declared;
    return declared;
}

// Whether something is mounted at `path`, by the test `mountpoint(1)` uses: a
// mount point and the directory holding it are on different devices, because
// the mount is what put a different filesystem there. It answers for every kind
// of mount a person can make -- a disk, a bind, sshfs, rclone, gvfs -- because
// all of them are a filesystem of their own.
bool isMountPoint(const QString &path)
{
    struct stat here {};
    struct stat above {};
    if (::stat(path.toLocal8Bit().constData(), &here) != 0)
        return false;
    const QString parent = QFileInfo(path).absolutePath();
    if (parent == path)
        return true;
    if (::stat(parent.toLocal8Bit().constData(), &above) != 0)
        return false;
    return here.st_dev != above.st_dev;
}

} // namespace

QString normalizeRootId(const QString &raw, QString *error)
{
    const QString folded = raw.trimmed().toLower();
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
            *error = QStringLiteral("roots: %1 is not an id; letters, digits, `-`, `_` "
                                    "and `.`, starting with a letter or a digit, at "
                                    "most 64").arg(raw);
        return {};
    }
    return folded;
}

QString defaultRootsPath()
{
    QString configHome = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME"));
    if (configHome.isEmpty())
        configHome = QDir::homePath() + QStringLiteral("/.config");
    return QDir(configHome).filePath(QStringLiteral("omafiles/roots.jsonl"));
}

const QVector<Root> &roots()
{
    return table();
}

QVector<Root> setRoots(QVector<Root> declared)
{
    QVector<Root> previous = table();
    table() = std::move(declared);
    return previous;
}

bool loadRoots(const QString &path, QString *error)
{
    QVector<Root> declared;
    QFile file(path);
    if (!file.exists()) {
        setRoots(declared);
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("roots: cannot read %1").arg(path);
        return false;
    }

    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            if (error)
                *error = QStringLiteral("roots: malformed jsonl at %1:%2").arg(path).arg(lineNumber);
            return false;
        }
        const QJsonObject object = doc.object();
        Root root;
        root.id = object.value(QStringLiteral("id")).toString();
        root.path = object.value(QStringLiteral("path")).toString();
        root.mount = object.value(QStringLiteral("mount")).toBool();
        root.check = object.value(QStringLiteral("check")).toString();
        if (root.id.isEmpty() || root.path.isEmpty()) {
            if (error)
                *error = QStringLiteral("roots: %1:%2 needs an id and a path").arg(path).arg(lineNumber);
            return false;
        }
        // Absolute, and without a trailing slash, because every comparison
        // below is a string one: `/mnt/nas/` and `/mnt/nas` would be two roots
        // holding the same files.
        root.path = QDir::cleanPath(QDir(root.path).absolutePath());
        for (const Root &already : std::as_const(declared)) {
            if (already.id == root.id) {
                if (error)
                    *error = QStringLiteral("roots: %1 is declared twice").arg(root.id);
                return false;
            }
        }
        declared.append(root);
    }
    setRoots(declared);
    return true;
}

bool loadRoots(QString *error)
{
    return loadRoots(defaultRootsPath(), error);
}

bool saveRoots(const QString &path, const QVector<Root> &declared, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("roots: cannot write %1").arg(path);
        return false;
    }
    QTextStream out(&file);
    for (const Root &root : declared) {
        QJsonObject object{
            {QStringLiteral("id"), root.id},
            {QStringLiteral("path"), root.path},
        };
        if (root.mount)
            object.insert(QStringLiteral("mount"), true);
        if (!root.check.isEmpty())
            object.insert(QStringLiteral("check"), root.check);
        out << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)) << '\n';
    }
    out.flush();
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("roots: cannot commit %1").arg(path);
        return false;
    }
    return true;
}

const Root *rootById(const QString &id)
{
    for (const Root &root : roots()) {
        if (root.id == id)
            return &root;
    }
    return nullptr;
}

const Root *rootFor(const QString &absolutePath)
{
    const Root *best = nullptr;
    for (const Root &root : roots()) {
        if (!isUnderDirectory(absolutePath, root.path) && absolutePath != root.path)
            continue;
        // The longest match, so a root declared inside another one takes the
        // rows under it rather than losing them to the one above.
        if (!best || root.path.size() > best->path.size())
            best = &root;
    }
    return best;
}

// Three answers, weakest last.
//
// A declared `check` is the sure one: it is a name that only exists when the
// storage does. Without it, a mount root is here when something is mounted
// where it says. And a root that is simply part of the filesystem is here when
// its directory is -- which is the same test as before roots existed, so
// declaring one over a folder that is always present changes nothing.
bool rootIsReachable(const Root &root)
{
    if (!root.check.isEmpty())
        return QFileInfo::exists(QDir(root.path).filePath(root.check));
    if (root.mount)
        return isMountPoint(root.path);
    return QFileInfo::exists(root.path);
}

bool underUnreachableRoot(const QString &absolutePath)
{
    const Root *root = rootFor(absolutePath);
    return root && !rootIsReachable(*root);
}

QString unreachableReason(const QString &absolutePath)
{
    const Root *root = rootFor(absolutePath);
    if (!root || rootIsReachable(*root))
        return {};
    return QStringLiteral("%1: root %2 is declared and not here")
        .arg(absolutePath, root->id);
}

void splitByRoot(const QString &absolutePath, QString *rootId, QString *relative)
{
    const Root *root = rootFor(absolutePath);
    if (!root) {
        if (rootId)
            rootId->clear();
        if (relative)
            relative->clear();
        return;
    }
    if (rootId)
        *rootId = root->id;
    if (relative) {
        // `.` for the root itself, so that the pair always spells something and
        // an empty relative can go on meaning "no root".
        *relative = absolutePath == root->path
            ? QStringLiteral(".")
            : absolutePath.mid(root->path.size() + 1);
    }
}

QString joinRoot(const QString &rootId, const QString &relative)
{
    const Root *root = rootById(rootId);
    if (!root)
        return {};
    if (relative == QLatin1String("."))
        return root->path;
    return QDir(root->path).filePath(relative);
}

} // namespace omafiles
