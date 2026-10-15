#include "ArchiveWorker.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace omafiles {
namespace {
struct State {
    qint64 bytes = 0, entries = 0;
    QElapsedTimer elapsed;
    ArchiveProgress progress;
    void report() {
        if (!elapsed.isValid() || elapsed.elapsed() >= 100) {
            elapsed.start();
            progress(entries, bytes);
        }
    }
};
using Reader = std::unique_ptr<archive, decltype(&archive_read_free)>;
using Writer = std::unique_ptr<archive, decltype(&archive_write_free)>;
using Entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void check(int status, archive *handle) {
    if (status != ARCHIVE_OK)
        fail(QString::fromLocal8Bit(archive_error_string(handle) ? archive_error_string(handle) : "Archive operation failed."));
}
QString nameOf(const char *name) {
    if (!name) fail("Archive entry has no name.");
    const QByteArray bytes(name);
    const QString decoded = QFile::decodeName(bytes);
    if (QFile::encodeName(decoded) != bytes) fail("Archive entry has an unreadable filename.");
    return decoded;
}
QString relativeName(const QString &name) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\'))
        fail("Archive contains an unsafe absolute or ambiguous path: " + name);
    const auto parts = name.split('/');
    if (parts.contains("..")) fail("Archive contains a parent-directory path: " + name);
    return QDir::cleanPath(name);
}
void validateLink(const QString &name, const char *target, bool hardlink) {
    const QString link = nameOf(target);
    if (link.isEmpty() || link.startsWith('/') || link.contains('\\'))
        fail("Archive contains an unsafe link: " + name);
    const QString resolved = hardlink ? relativeName(link)
        : QDir::cleanPath(QFileInfo(name).path() + '/' + link);
    if (resolved == ".." || resolved.startsWith("../"))
        fail("Archive link leaves the extraction directory: " + name);
}
void safeMetadata(archive_entry *entry, bool extracting = false) {
    QByteArray tags;
    bool hasTags = false;
    archive_entry_xattr_reset(entry);
    const char *name = nullptr;
    const void *value = nullptr;
    size_t size = 0;
    while (archive_entry_xattr_next(entry, &name, &value, &size) == ARCHIVE_OK) {
        if (name && std::strcmp(name, "user.omafiles.tags") == 0) {
            if (size > 65536) fail("Archive tag metadata is too large.");
            tags = QByteArray(static_cast<const char *>(value), qsizetype(size));
            hasTags = true;
        }
    }
    archive_entry_xattr_clear(entry);
    if (hasTags) archive_entry_xattr_add_entry(entry, "user.omafiles.tags", tags.constData(), size_t(tags.size()));
    archive_entry_acl_clear(entry);
    archive_entry_set_fflags(entry, 0, 0);
    mode_t mode = archive_entry_perm(entry) & 0777;
    if (extracting) {
        if (archive_entry_filetype(entry) == AE_IFDIR) mode |= 0700;
        if (archive_entry_filetype(entry) == AE_IFREG) mode |= 0400;
    }
    archive_entry_set_perm(entry, mode);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_uname(entry, nullptr);
    archive_entry_set_gname(entry, nullptr);
}
void safeType(archive_entry *entry) {
    const auto type = archive_entry_filetype(entry);
    if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK
        && !(type == 0 && archive_entry_hardlink(entry)))
        fail("Archive contains an unsupported device, socket, or special file.");
}
void extractArchive(const QString &path, const QString &stage, const std::shared_ptr<State> &state) {
    Reader reader(archive_read_new(), archive_read_free);
    Writer writer(archive_write_disk_new(), archive_write_free);
    check(archive_read_support_filter_none(reader.get()), reader.get());
    check(archive_read_support_filter_gzip(reader.get()), reader.get());
    check(archive_read_support_filter_bzip2(reader.get()), reader.get());
    check(archive_read_support_filter_xz(reader.get()), reader.get());
    check(archive_read_support_filter_zstd(reader.get()), reader.get());
    check(archive_read_support_filter_lz4(reader.get()), reader.get());
    check(archive_read_support_format_all(reader.get()), reader.get());
    check(archive_read_open_filename(reader.get(), QFile::encodeName(path).constData(), 65536), reader.get());
    check(archive_write_disk_set_options(writer.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM
        | ARCHIVE_EXTRACT_XATTR | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT), writer.get());
    QSet<QString> declared;
    archive_entry *entry = nullptr;
    for (;;) {
        state->report();
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        check(status, reader.get());
        const QString name = relativeName(nameOf(archive_entry_pathname(entry)));
        safeType(entry);
        if (name == ".") {
            if (archive_entry_filetype(entry) != AE_IFDIR) fail("Archive root entry is not a directory.");
            check(archive_read_data_skip(reader.get()), reader.get());
            continue;
        }
        if (declared.contains(name)) fail("Archive contains duplicate entries: " + name);
        declared.insert(name);
        if (const char *link = archive_entry_symlink(entry)) validateLink(name, link, false);
        if (const char *link = archive_entry_hardlink(entry)) {
            validateLink(name, link, true);
            const QString target = QDir(stage).filePath(relativeName(nameOf(link)));
            QString ancestor = stage;
            for (const QString &part : relativeName(nameOf(link)).split('/')) {
                ancestor += '/' + part;
                if (QFileInfo(ancestor).isSymLink()) fail("Archive hard link crosses a symbolic link: " + name);
            }
            const QFileInfo info(target);
            if (!info.isFile() || info.isSymLink()) fail("Archive hard link must refer to an earlier regular file: " + name);
            archive_entry_set_hardlink(entry, QFile::encodeName(relativeName(nameOf(link))).constData());
            archive_entry_set_filetype(entry, AE_IFREG);
        }
        safeMetadata(entry, true);
        archive_entry_set_pathname(entry, QFile::encodeName(name).constData());
        check(archive_write_header(writer.get(), entry), writer.get());
        for (;;) {
            state->report();
            const void *buffer = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            const int data = archive_read_data_block(reader.get(), &buffer, &size, &offset);
            if (data == ARCHIVE_EOF) break;
            check(data, reader.get());
            check(archive_write_data_block(writer.get(), buffer, size, offset), writer.get());
            state->bytes += qint64(size);
            state->report();
        }
        check(archive_write_finish_entry(writer.get()), writer.get());
        ++state->entries;
    }
    check(archive_read_close(reader.get()), reader.get());
    check(archive_write_close(writer.get()), writer.get());
}
void compressArchive(const QStringList &paths, const QString &output, const QString &format,
                     const std::shared_ptr<State> &state) {
    Writer writer(archive_write_new(), archive_write_free);
    if (format == "zip") check(archive_write_set_format_zip(writer.get()), writer.get());
    else {
        check(archive_write_set_format_pax_restricted(writer.get()), writer.get());
        if (format == "tar.gz") check(archive_write_add_filter_gzip(writer.get()), writer.get());
        else if (format == "tar.zst") check(archive_write_add_filter_zstd(writer.get()), writer.get());
        else fail("Choose ZIP, tar.gz, or tar.zst.");
    }
    check(archive_write_open_filename(writer.get(), QFile::encodeName(output).constData()), writer.get());
    QSet<QString> names;
    QByteArray buffer(65536, Qt::Uninitialized);
    for (const QString &path : paths) {
        state->report();
        const QString absolute = QFileInfo(path).absoluteFilePath();
        const QString parent = QFileInfo(absolute).absolutePath();
        const QString relativeOutput = QDir(absolute).relativeFilePath(output);
        if (QFileInfo(absolute).isDir() && relativeOutput != ".." && !relativeOutput.startsWith("../"))
            fail("Archive destination must be outside the selected folders.");
        Reader disk(archive_read_disk_new(), archive_read_free);
        check(archive_read_disk_set_symlink_physical(disk.get()), disk.get());
        check(archive_read_disk_open(disk.get(), QFile::encodeName(absolute).constData()), disk.get());
        Entry entry(archive_entry_new(), archive_entry_free);
        for (;;) {
            state->report();
            archive_entry_clear(entry.get());
            const int status = archive_read_next_header2(disk.get(), entry.get());
            if (status == ARCHIVE_EOF) break;
            check(status, disk.get());
            safeType(entry.get());
            const QString source = nameOf(archive_entry_pathname(entry.get()));
            const QString name = relativeName(QDir(parent).relativeFilePath(source));
            if (name == "." || names.contains(name)) fail("Selected archive paths overlap or have duplicate names: " + name);
            names.insert(name);
            if (const char *link = archive_entry_symlink(entry.get())) validateLink(name, link, false);
            QFile file;
            struct stat before {};
            if (archive_entry_filetype(entry.get()) == AE_IFREG) {
                const int fd = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if (fd < 0) fail("Cannot read " + source + ": " + QString::fromLocal8Bit(std::strerror(errno)));
                if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                    ::close(fd);
                    fail("Cannot read " + source + ": " + file.errorString());
                }
                if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)
                    || quint64(before.st_ino) != quint64(archive_entry_ino64(entry.get())) || before.st_dev != archive_entry_dev(entry.get())
                    || before.st_size != archive_entry_size(entry.get()))
                    fail("Source changed while preparing the archive: " + source);
            }
            safeMetadata(entry.get());
            archive_entry_set_hardlink(entry.get(), nullptr);
            archive_entry_set_pathname(entry.get(), QFile::encodeName(name).constData());
            check(archive_write_header(writer.get(), entry.get()), writer.get());
            qint64 remaining = archive_entry_filetype(entry.get()) == AE_IFREG ? archive_entry_size(entry.get()) : 0;
            while (remaining > 0) {
                state->report();
                const qint64 count = file.read(buffer.data(), qMin(remaining, qint64(buffer.size())));
                if (count <= 0) fail("Source could not be read completely: " + source);
                qint64 position = 0;
                while (position < count) {
                    const la_ssize_t written = archive_write_data(writer.get(), buffer.constData() + position, size_t(count - position));
                    if (written <= 0) fail(QString::fromLocal8Bit(archive_error_string(writer.get())));
                    position += written;
                }
                remaining -= count;
                state->bytes += count;
                state->report();
            }
            if (file.isOpen()) {
                struct stat after {};
                if (::fstat(file.handle(), &after) != 0 || !file.atEnd() || before.st_size != after.st_size
                    || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
                    || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
                    fail("Source changed while preparing the archive: " + source);
            }
            check(archive_write_finish_entry(writer.get()), writer.get());
            check(archive_read_disk_descend(disk.get()), disk.get());
            ++state->entries;
        }
        check(archive_read_close(disk.get()), disk.get());
    }
    check(archive_write_close(writer.get()), writer.get());
}
}
QStringList runArchive(const QStringList &paths, const QString &destination, const QString &format,
                       bool extracting, const QString &stage, const ArchiveProgress &progress) {
    if (paths.isEmpty() || destination.isEmpty()) fail("Select files and an archive destination.");
    for (const auto &path : paths)
        if (!QDir::isAbsolutePath(path) || path.contains(QChar(0))) fail("Archive source must be an absolute path.");
    if (!QDir::isAbsolutePath(destination) || destination.contains(QChar(0))) fail("Archive destination must be an absolute path.");
    struct stat directoryState {};
    if (!QDir::isAbsolutePath(stage) || stage.contains(QChar(0))
        || ::lstat(QFile::encodeName(stage).constData(), &directoryState) != 0
        || !S_ISDIR(directoryState.st_mode) || directoryState.st_uid != ::geteuid()
        || (directoryState.st_mode & 0777) != 0700)
        fail("Archive staging directory must be private and owned by this user.");
    if (!QDir::setCurrent(stage)) fail("Cannot enter private archive staging directory.");
    auto state = std::make_shared<State>();
    state->progress = progress;
    if (extracting) extractArchive(paths.first(), stage, state);
    else {
        const QString filename = QFileInfo(destination).fileName();
        if (filename.isEmpty() || filename == "." || filename == "..") fail("Choose an archive filename.");
        compressArchive(paths, QDir(stage).filePath(filename), format, state);
    }
    QStringList names;
    DIR *directory = ::opendir(QFile::encodeName(stage).constData());
    if (!directory) fail("Cannot list the prepared archive files.");
    const auto closeDirectory = [](DIR *handle) { ::closedir(handle); };
    std::unique_ptr<DIR, decltype(closeDirectory)> holder(directory, closeDirectory);
    errno = 0;
    while (const dirent *entry = ::readdir(directory)) {
        const QByteArray raw(entry->d_name);
        if (raw == "." || raw == "..") continue;
        const QString name = nameOf(entry->d_name);
        names.append(name);
        errno = 0;
    }
    if (errno) fail("Cannot list the prepared archive files completely.");
    names.sort();
    progress(state->entries, state->bytes);
    return names;
}
}
