#include "DefaultManager.h"
#include "Open.h"
#include "Paths.h"

#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QScopeGuard>

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omafiles {
namespace {
class Registration
{
public:
    explicit Registration(QString path, bool backup = true, bool temporary = false)
        : m_path(std::move(path)), m_temporary(temporary)
    {
        QString name;
        m_directory = typedDirectory(m_path, &name);
        m_name = QFile::encodeName(name);
        if (backup)
            m_backup = m_name + ".omafiles-backup";
    }
    ~Registration() { if (m_fd >= 0) ::close(m_fd); }
    Registration(const Registration &) = delete;
    Registration &operator=(const Registration &) = delete;

    bool prepare(QString *error)
    {
        // Pinned parent descriptors keep replacement from redirecting writes.
        m_fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (m_fd < 0)
            return failed(error, m_directory);
        QString prefix;
        for (const QString &part : m_directory.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            prefix += QLatin1Char('/') + part;
            const QByteArray name = QFile::encodeName(part);
            struct stat st {};
            if (::fstatat(m_fd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0
                && S_ISLNK(st.st_mode)) {
                *error = QStringLiteral("Refusing symbolic link: %1").arg(prefix);
                return false;
            }
            if (::mkdirat(m_fd, name.constData(), 0777) != 0 && errno != EEXIST)
                return failed(error, prefix);
            const int next = ::openat(m_fd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0)
                return failed(error, prefix);
            ::close(m_fd);
            m_fd = next;
        }
        return check(m_name, error) && (m_backup.isEmpty() || check(m_backup, error));
    }

    bool install(const QByteArray &contents, QStringList *receipt)
    {
        QString error;
        QByteArray previous;
        struct stat st {};
        const int source = ::openat(m_fd, m_name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        const bool exists = source >= 0;
        if (!exists && errno != ENOENT)
            failed(&error, m_path);
        if (exists) {
            QFile original;
            if (::fstat(source, &st) != 0 || !S_ISREG(st.st_mode)) {
                error = QStringLiteral("Cannot read regular registration: %1").arg(m_path);
                ::close(source);
            } else if (!original.open(source, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                ::close(source);
                error = QStringLiteral("Cannot read %1: %2").arg(m_path, original.errorString());
            } else {
                previous = original.readAll();
                if (original.error() != QFileDevice::NoError)
                    error = QStringLiteral("Cannot read %1: %2").arg(m_path, original.errorString());
            }
        }
        const bool same = exists && previous == contents;
        if (error.isEmpty() && !same) {
            struct stat saved {};
            if (exists && ::fstatat(m_fd, m_backup.constData(), &saved, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno != ENOENT || !writeNew(m_backup, previous, st.st_mode & 0777, &error)) {
                    if (error.isEmpty())
                        failed(&error, m_path + QStringLiteral(".omafiles-backup"));
                }
            }
            if (error.isEmpty()) {
                const QByteArray temp = m_name + '.' + QByteArray::number(QRandomGenerator::global()->generate64(), 36);
                if (writeNew(temp, contents, exists ? st.st_mode & 0777 : 0666, &error, exists)) {
                    const auto cleanup = qScopeGuard([&] { ::unlinkat(m_fd, temp.constData(), 0); });
                    if (!check(m_name, &error) || ::renameat(m_fd, temp.constData(), m_fd, m_name.constData()) != 0) {
                        if (error.isEmpty())
                            failed(&error, m_path);
                    }
                }
            }
        }
        if (!error.isEmpty()) {
            receipt->append(error);
            return false;
        }
        receipt->append(QStringLiteral("%1 %2").arg(same ? QStringLiteral("Already installed:")
                                                           : QStringLiteral("Installed:"), m_path));
        struct stat saved {};
        receipt->append(::fstatat(m_fd, m_backup.constData(), &saved, AT_SYMLINK_NOFOLLOW) == 0
            ? QStringLiteral("Undo: mv -- %1 %2").arg(shellQuoted(m_path + QStringLiteral(".omafiles-backup")), shellQuoted(m_path))
            : QStringLiteral("Undo: rm -- %1").arg(shellQuoted(m_path)));
        return true;
    }

private:
    bool failed(QString *error, const QString &path) const
    {
        *error = QStringLiteral("Cannot prepare or write %1: %2").arg(path, systemReason(errno));
        return false;
    }
    bool check(const QByteArray &name, QString *error) const
    {
        struct stat st {};
        if (::fstatat(m_fd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) != 0)
            return errno == ENOENT || failed(error, m_path);
        if (S_ISREG(st.st_mode) && !m_temporary)
            return true;
        const QString path = name == m_backup ? m_path + QStringLiteral(".omafiles-backup") : m_path;
        *error = QStringLiteral("Refusing %1: %2").arg(path,
            S_ISLNK(st.st_mode) ? QStringLiteral("symbolic link")
                : m_temporary && S_ISREG(st.st_mode) ? QStringLiteral("temporary path already exists")
                                                   : QStringLiteral("not a regular file"));
        return false;
    }
    bool writeNew(const QByteArray &name, const QByteArray &contents, mode_t mode, QString *error,
                  bool preserveMode = true) const
    {
        const int fd = ::openat(m_fd, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd < 0)
            return failed(error, m_path);
        QFile file;
        int failure = 0;
        if (!file.open(fd, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)
            || file.write(contents) != contents.size() || !file.flush())
            failure = errno ? errno : EIO;
        if (!failure && preserveMode && ::fchmod(fd, mode) != 0)
            failure = errno;
        if (!failure && ::fsync(fd) != 0)
            failure = errno;
        file.close();
        if (::close(fd) != 0 && !failure)
            failure = errno;
        if (!failure)
            return true;
        ::unlinkat(m_fd, name.constData(), 0);
        *error = QStringLiteral("Cannot write %1: %2").arg(m_path, systemReason(failure));
        return false;
    }
    QString m_path;
    QString m_directory;
    QByteArray m_name;
    QByteArray m_backup;
    bool m_temporary;
    int m_fd = -1;
};
} // namespace

DefaultManagerResult makeDefaultFileManager(const QString &executable)
{
    QString error;
    if (!environmentNameable(&error))
        return {false, error};
    QStringList receipt;
    QFile desktop(QStringLiteral(":/omafiles.desktop"));
    if (!desktop.open(QIODevice::ReadOnly))
        return {false, QStringLiteral("Cannot read the bundled desktop entry")};
    const QString reserved = QStringLiteral("\"'\\><~|&;$*?#()`%=");
    bool unsupported = !QDir::isAbsolutePath(executable);
    for (const QChar character : executable)
        unsupported |= character.isSpace() || character.category() == QChar::Other_Control
            || reserved.contains(character);
    if (unsupported)
        return {false, QStringLiteral("Cannot make this executable the default: its path must be absolute "
            "and contain no reserved Desktop Entry characters (including spaces, quotes, %, or =). "
            "Run mise run install, then start the installed omafiles-studio from PATH and retry.")};
    QByteArray entry = desktop.readAll();
    entry.replace("Exec=omafiles-studio %U", "Exec=" + executable.toUtf8() + " %U");
    QFile activation(QStringLiteral(":/filemanager.service"));
    if (!activation.open(QIODevice::ReadOnly))
        return {false, QStringLiteral("Cannot read the bundled activation service")};
    QByteArray service = activation.readAll();
    service.replace("Exec=/usr/bin/omafiles-studio", "Exec=" + executable.toUtf8());
    Registration desktopFile(dataHome() + QStringLiteral("/applications/omafiles.desktop"));
    Registration serviceFile(dataHome() + QStringLiteral("/dbus-1/services/org.freedesktop.FileManager1.service"));
    Registration mimeFile(configHome() + QStringLiteral("/mimeapps.list"), false);
    Registration mimeTemp(configHome() + QStringLiteral("/mimeapps.list.new"), false, true);
    Registration kdeMime(dataHome() + QStringLiteral("/applications/mimeapps.list"), false);
    Registration kdeTemp(dataHome() + QStringLiteral("/applications/mimeapps.list.new"), false, true);
    for (Registration *file : {&desktopFile, &serviceFile, &mimeFile, &mimeTemp, &kdeMime, &kdeTemp}) {
        if (!file->prepare(&error))
            return {false, error};
    }
    if (!desktopFile.install(entry, &receipt) || !serviceFile.install(service, &receipt))
        return {false, receipt.join(QLatin1Char('\n'))};
    if (!setDirectoryDefault(&error)) {
        receipt.append(error);
        receipt.append(QStringLiteral("MIME selection failed; registrations above remain installed."));
        return {false, receipt.join(QLatin1Char('\n'))};
    }
    receipt.append(QStringLiteral("Selected omafiles.desktop for inode/directory.\n"
        "Undo the MIME choice: xdg-mime default OTHER.desktop inode/directory\n"
        "Replace OTHER.desktop with your preferred file manager.\n"
        "Close other file managers to release their running FileManager1 service."));
    return {true, receipt.join(QLatin1Char('\n'))};
}
} // namespace omafiles
