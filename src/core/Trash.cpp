#include "Trash.h"

#include "Paths.h"
#include "Store.h"

#include <QDateTime>
#include <QDir>
#include <QSet>
#include <QStorageInfo>
#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace omafiles::Trash {
namespace {

std::mutex providerMutex;
MountProvider mountProvider;

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value(value) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int value;
};

QString refusal(const QString &path, int failure)
{
    return QStringLiteral("Trash: %1: %2").arg(spelled(path), systemReason(failure));
}


QList<Location> candidates(const QStringList &mounts)
{
    QList<Location> result{{QDir(dataHome()).filePath(QStringLiteral("Trash")), dataHome()}};
    QSet<QString> seen{result.first().path};
    const QString uid = QString::number(::getuid());
    for (const QString &mount : mounts) {
        if (!isCleanAbsolute(mount))
            continue;
        struct stat state{};
        const PathState present = statState(mount, nullptr, false, &state);
        if (present == PathState::Missing || (present == PathState::Present && !S_ISDIR(state.st_mode)))
            continue;
        for (const QString &suffix : {QStringLiteral(".Trash/") + uid,
                                      QStringLiteral(".Trash-") + uid}) {
            const QString path = QDir(mount).filePath(suffix);
            if (!seen.contains(path)) {
                result.append({path, mount});
                seen.insert(path);
            }
        }
    }
    return result;
}

int root(const Location &location, QString *error)
{
    const QString home = QDir(dataHome()).filePath(QStringLiteral("Trash"));
    const QString uid = QString::number(::getuid());
    const bool shared = location.path == QDir(location.top).filePath(QStringLiteral(".Trash/") + uid);
    if ((location.path == home && location.top != dataHome())
        || (location.path != home && !shared
        && location.path != QDir(location.top).filePath(QStringLiteral(".Trash-") + uid))) {
        *error = QStringLiteral("Trash: invalid trash location");
        return -1;
    }
    if (storageState(location.path, error) != PathState::Present)
        return -1;
    if (shared) {
        const QString parent = QDir(location.top).filePath(QStringLiteral(".Trash"));
        Descriptor fd(openDirectoryNoFollow(parent));
        struct stat state{};
        if (fd.value < 0) {
            if (errno != ENOENT)
                *error = refusal(parent, errno);
            return -1;
        }
        if (::fstat(fd.value, &state) != 0) {
            *error = refusal(parent, errno);
            return -1;
        }
        if (!(state.st_mode & S_ISVTX)) {
            *error = QStringLiteral("Trash: shared trash directory has no sticky bit: %1").arg(spelled(parent));
            return -1;
        }
    }
    const int fd = openDirectoryNoFollow(location.path);
    if (fd < 0) {
        if (errno != ENOENT)
            *error = refusal(location.path, errno);
        return -1;
    }
    struct stat state{};
    if (::fstat(fd, &state) != 0 || state.st_uid != ::getuid()) {
        *error = QStringLiteral("Trash: trash directory is not owned by the current user: %1")
                     .arg(spelled(location.path));
        ::close(fd);
        return -1;
    }
    return fd;
}

int childDirectory(int parent, const QString &path, const char *name, QString *error)
{
    const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *error = refusal(path + u'/' + QLatin1String(name), errno);
        return -1;
    }
    struct stat state{};
    if (::fstat(fd, &state) != 0 || state.st_uid != ::getuid()) {
        *error = QStringLiteral("Trash: directory is not owned by the current user: %1")
                     .arg(spelled(path + u'/' + QLatin1String(name)));
        ::close(fd);
        return -1;
    }
    return fd;
}

QStringList names(int fd, const QString &path, QString *error,
                  const std::shared_ptr<std::atomic_bool> &cancel = {})
{
    QStringList result, unnameable;
    visitDirectory(fd, path, [&](QAnyStringView name, unsigned char) {
        if (cancel && cancel->load())
            return false;
        result.append(name.toString());
        return true;
    }, &unnameable, error);
    if (!unnameable.isEmpty()) {
        if (!error->isEmpty())
            *error += QStringLiteral("; ");
        *error += QStringLiteral("Trash contains names that are not valid UTF-8: ")
            + unnameable.join(QStringLiteral(", "));
    }
    return result;
}

bool validName(const QString &name)
{
    return !name.isEmpty() && name != u"." && name != u".." && !name.contains(u'/') && !name.contains(QChar::Null);
}

QByteArray signature(const struct stat &state)
{
    return QByteArray::number(state.st_dev) + ':' + QByteArray::number(state.st_ino) + ':'
        + QByteArray::number(state.st_mode) + ':' + QByteArray::number(state.st_size) + ':'
        + QByteArray::number(state.st_mtim.tv_sec) + ':' + QByteArray::number(state.st_mtim.tv_nsec)
        + ':' + QByteArray::number(state.st_ctim.tv_sec) + ':' + QByteArray::number(state.st_ctim.tv_nsec);
}

Item readAt(const Location &location, int files, int infos, const QString &name)
{
    Item item;
    item.location = location;
    item.path = location.path + QStringLiteral("/files/") + name;
    item.info = location.path + QStringLiteral("/info/") + name + QStringLiteral(".trashinfo");
    if (!validName(name)) {
        item.error = QStringLiteral("Trash: invalid entry name");
        return item;
    }
    struct stat payload{};
    if (::fstatat(files, QFile::encodeName(name).constData(), &payload, AT_SYMLINK_NOFOLLOW) == 0) {
        item.fileDevice = payload.st_dev;
        item.fileInode = payload.st_ino;
        item.signature = signature(payload);
    } else {
        item.error = refusal(item.path, errno);
    }
    const QByteArray infoName = QFile::encodeName(name + QStringLiteral(".trashinfo"));
    struct stat identity{};
    if (::fstatat(infos, infoName.constData(), &identity, AT_SYMLINK_NOFOLLOW) == 0) {
        item.device = identity.st_dev;
        item.inode = identity.st_ino;
        item.metadataSignature = signature(identity);
    }
    Descriptor info(::openat(infos, infoName.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (info.value < 0) {
        item.error = refusal(item.info, errno);
        return item;
    }
    struct stat state{};
    if (::fstat(info.value, &state) != 0 || !S_ISREG(state.st_mode) || state.st_size > 65536) {
        item.error = QStringLiteral("Trash: information file must be a regular file of at most 64 KiB: %1")
                         .arg(spelled(item.info));
        return item;
    }
    item.device = state.st_dev;
    item.inode = state.st_ino;
    item.metadataSignature = signature(state);
    QFile content;
    if (!content.open(info.value, QIODevice::ReadOnly, QFileDevice::DontCloseHandle)) {
        item.error = refusal(item.info, EIO);
        return item;
    }
    item.metadata = content.read(65537);
    item.metadata.squeeze();
    if (content.error() != QFileDevice::NoError || item.metadata.size() > 65536) {
        item.error = QStringLiteral("Trash: cannot read information file: %1").arg(spelled(item.info));
        return item;
    }
    const auto lines = item.metadata.split('\n');
    auto trimmedLine = [](QByteArray line) { if (line.endsWith('\r')) line.chop(1); return line; };
    if (lines.isEmpty() || trimmedLine(lines.first()) != "[Trash Info]") {
        item.error = QStringLiteral("Trash: invalid information file header: %1").arg(spelled(item.info));
        return item;
    }
    QByteArray path, date;
    bool pathSeen = false, dateSeen = false;
    for (const QByteArray &raw : lines) {
        const QByteArray line = trimmedLine(raw);
        if (!pathSeen && line.startsWith("Path=")) {
            path = line.mid(5);
            pathSeen = true;
        } else if (!dateSeen && line.startsWith("DeletionDate=")) {
            date = line.mid(13);
            dateSeen = true;
        }
    }
    for (qsizetype at = 0; at < path.size(); ++at) {
        if (path.at(at) != '%')
            continue;
        auto hex = [](char byte) { return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F'); };
        if (at + 2 >= path.size() || !hex(path.at(at + 1)) || !hex(path.at(at + 2))) {
            item.error = QStringLiteral("Trash: invalid percent escape in original path: %1").arg(spelled(item.info));
            return item;
        }
        at += 2;
    }
    QString original;
    if (path.isEmpty() || !nameFromBytes(QByteArray::fromPercentEncoding(path), &original)
        || (!original.startsWith(u'/') && !isCleanRelative(original))
        || (original.startsWith(u'/') && !isCleanAbsolute(original))) {
        item.error = QStringLiteral("Trash: missing or unsafe original path: %1").arg(spelled(item.info));
        return item;
    }
    const bool home = location.path == QDir(dataHome()).filePath(QStringLiteral("Trash"));
    if (!original.startsWith(u'/'))
        original = QDir::cleanPath(QDir(location.top).filePath(original));
    if ((!home && !isUnderDirectory(original, location.top)) || original == location.top || original == u"/") {
        item.error = QStringLiteral("Trash: original path is outside its filesystem: %1").arg(spelled(item.info));
        return item;
    }
    const QString deleted = QString::fromLatin1(date);
    const QDateTime parsed = QDateTime::fromString(deleted, QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));
    if (!parsed.isValid() || parsed.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) != deleted) {
        item.error = QStringLiteral("Trash: missing or invalid deletion date: %1").arg(spelled(item.info));
        return item;
    }
    item.original = original;
    item.deleted = deleted;
    return item;
}

bool sameItem(const Item &before, const Item &now)
{
    return before.path == now.path && before.info == now.info && before.original == now.original
        && before.metadata == now.metadata && before.device == now.device && before.inode == now.inode
        && before.fileDevice == now.fileDevice && before.fileInode == now.fileInode
        && before.signature == now.signature && before.metadataSignature == now.metadataSignature;
}

struct Access {
    Descriptor fd;
    Descriptor files;
    Descriptor infos;
    QString error;
    QString name;

    explicit Access(const Item &item)
    {
        fd.value = root(item.location, &error);
        if (fd.value < 0) {
            if (error.isEmpty())
                error = QStringLiteral("Trash: trash directory is missing");
            return;
        }
        files.value = childDirectory(fd.value, item.location.path, "files", &error);
        infos.value = childDirectory(fd.value, item.location.path, "info", &error);
        if (files.value < 0 || infos.value < 0)
            return;
        name = item.path.mid(item.location.path.size() + 7);
        check(item);
    }

    bool check(const Item &item)
    {
        if (!validName(name) || !sameItem(item, readAt(item.location, files.value, infos.value, name)))
            error = QStringLiteral("Trash: entry changed since it was listed; refresh and try again");
        return error.isEmpty();
    }

    QString removeInfo(const Item &item)
    {
        const Item now = readAt(item.location, files.value, infos.value, name);
        if (item.metadataSignature != now.metadataSignature || item.metadata != now.metadata)
            return QStringLiteral("Trash: information file changed during the operation; it was kept");
        const QByteArray infoName = QFile::encodeName(name + QStringLiteral(".trashinfo"));
        if (::unlinkat(infos.value, infoName.constData(), 0) != 0 && errno != ENOENT)
            return refusal(item.info, errno);
        return {};
    }
};


} // namespace

void setMountProvider(MountProvider provider)
{
    std::lock_guard<std::mutex> lock(providerMutex);
    mountProvider = std::move(provider);
}

QStringList mounts()
{
    MountProvider provider;
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        provider = mountProvider;
    }
    if (provider)
        return provider();
    QStringList result;
    for (const auto &storage : QStorageInfo::mountedVolumes()) {
        struct stat state{};
        if (storage.isValid() && storage.isReady()
            && statState(storage.rootPath(), nullptr, false, &state) == PathState::Present
            && S_ISDIR(state.st_mode))
            result.append(storage.rootPath());
    }
    result.removeDuplicates();
    return result;
}

Listing list(const QStringList &mounts, const std::shared_ptr<std::atomic_bool> &cancel)
{
    Listing result;
    for (const auto &location : candidates(mounts)) {
        if (cancel && cancel->load())
            break;
        QString error;
        QString ancestor = location.top;
        while (isCleanAbsolute(ancestor)) {
            Descriptor watch(openDirectoryNoFollow(ancestor));
            if (watch.value >= 0) {
                result.watches.append(ancestor);
                break;
            }
            if (ancestor == u"/")
                break;
            ancestor = typedDirectory(ancestor, nullptr);
        }
        Descriptor fd(root(location, &error));
        if (fd.value < 0) {
            if (!error.isEmpty())
                result.errors.append(error);
            continue;
        }
        result.watches.append(location.path);
        Descriptor files(childDirectory(fd.value, location.path, "files", &error));
        Descriptor infos(childDirectory(fd.value, location.path, "info", &error));
        if (files.value >= 0)
            result.watches.append(location.path + QStringLiteral("/files"));
        if (infos.value >= 0)
            result.watches.append(location.path + QStringLiteral("/info"));
        if (files.value < 0 || infos.value < 0) {
            result.errors.append(error);
            continue;
        }
        const auto payloads = names(files.value, location.path + QStringLiteral("/files"), &error, cancel);
        if (!error.isEmpty())
            result.errors.append(location.path + QStringLiteral(": ") + error);
        error.clear();
        const auto information = names(infos.value, location.path + QStringLiteral("/info"), &error, cancel);
        if (!error.isEmpty())
            result.errors.append(location.path + QStringLiteral(": ") + error);
        QSet<QString> entries(payloads.begin(), payloads.end());
        for (const QString &name : information) {
            if (name.endsWith(QStringLiteral(".trashinfo")))
                entries.insert(name.chopped(10));
            else
                result.errors.append(QStringLiteral("Trash: unexpected information entry: %1").arg(spelled(location.path + QStringLiteral("/info/") + name)));
        }
        QStringList ordered(entries.begin(), entries.end());
        std::sort(ordered.begin(), ordered.end());
        for (const QString &name : ordered) {
            if (cancel && cancel->load())
                return result;
            result.items.append(readAt(location, files.value, infos.value, name));
        }
    }
    result.watches.removeDuplicates();
    return result;
}

QString restore(const Item &item)
{
    if (!item.error.isEmpty())
        return item.error;
    Access access(item);
    if (!access.error.isEmpty())
        return access.error;
    QString error;
    if (storageState(item.original, &error) != PathState::Present)
        return error;
    QString destination;
    const QString parent = typedDirectory(item.original, &destination);
    Descriptor target(openDirectoryNoFollow(parent));
    if (target.value < 0)
        return QStringLiteral("Trash: cannot restore original directory %1: %2. Create the directory or remove symbolic links from its path, then try again.")
            .arg(spelled(parent), systemReason(errno));
    Store store(defaultIndexPath());
    if (!store.lockAndLoad(&error))
        return error;
    if (!access.check(item))
        return access.error;
    const QByteArray sourceName = QFile::encodeName(access.name);
    const QByteArray targetName = QFile::encodeName(destination);
    if (::renameat2(access.files.value, sourceName.constData(), target.value, targetName.constData(), RENAME_NOREPLACE) != 0) {
        const int failure = errno;
        if (failure == EEXIST)
            return QStringLiteral("Trash: original path already exists: %1. Move or rename it before restoring.").arg(spelled(item.original));
        return refusal(item.original, failure);
    }
    for (Record row : store.records()) {
        if (!isUnderDirectory(row.path, item.path))
            continue;
        const QString old = row.path;
        row.path = item.original + row.path.mid(item.path.size());
        const Record original = store.get(row.path);
        if (!original.path.isEmpty()) {
            row.tags.append(original.tags);
            row.tagged = std::max(row.tagged, original.tagged);
        }
        row.root.clear();
        row.relative.clear();
        store.remove(old);
        store.put(row);
    }
    if (!store.commit(&error)) {
        if (::renameat2(target.value, targetName.constData(), access.files.value, sourceName.constData(), RENAME_NOREPLACE) != 0)
            error += QStringLiteral("; restored file could not be returned to Trash: ") + systemReason(errno);
        return error;
    }
    error = access.removeInfo(item);
    if (!error.isEmpty())
        return QStringLiteral("Trash: restored file and tags, but ") + error;
    return {};
}

QString erase(const Item &item)
{
    Access access(item);
    if (!access.error.isEmpty())
        return access.error;
    QString error;
    Store store(defaultIndexPath());
    if (!store.lockAndLoad(&error))
        return error;
    if (!access.check(item))
        return access.error;
    if (item.fileInode) {
        error = removeTreeAt(access.files.value, QFile::encodeName(access.name));
        if (!error.isEmpty())
            return QStringLiteral("Trash: could not erase %1: %2; information file was kept").arg(spelled(item.path), error);
    }
    error = access.removeInfo(item);
    if (!error.isEmpty())
        return error;
    for (const Record &row : store.records()) {
        if (isUnderDirectory(row.path, item.path))
            store.remove(row.path);
    }
    if (!store.commit(&error))
        return QStringLiteral("Trash: files were erased, but the tag index could not be updated: ") + error;
    return {};
}

} // namespace omafiles::Trash
