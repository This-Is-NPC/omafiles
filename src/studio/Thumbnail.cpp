#include "Thumbnail.h"
#include "../core/CpuBudget.h"
#include "HelperProcess.h"
#include "Open.h"
#include "Paths.h"

#include <QBuffer>
#include <QColorSpace>
#include <QtEndian>
#include <zlib.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeDatabase>
#include <QMutex>
#include <QSet>
#include <QCoreApplication>
#include <algorithm>
#include <utility>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QtMath>
#include <QPointer>
#include <QProcess>
#include <QHash>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUrl>
#include <QUuid>
#include <QtConcurrentRun>
#include <fcntl.h>
#include "EntryWatcher.h"
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace omafiles {
namespace {
QMutex runnerMutex;
ThumbnailRunner runner;
thread_local qint64 helperBudget = helperMemoryLimit;
thread_local int helperCpu = -1;

void initializeImageLimit() { QImageReader::setAllocationLimit(32); }
Q_COREAPP_STARTUP_FUNCTION(initializeImageLimit)

QString cacheRoot()
{
    return QDir::cleanPath(cacheHome()) + "/thumbnails";
}

// Held directory descriptors keep every cache operation away from symlink targets.
int directory(const QString &path, bool create)
{
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    QString walked;
    for (const QString &part : path.split('/', Qt::SkipEmptyParts)) {
        const QByteArray name = QFile::encodeName(part);
        if (create && ::mkdirat(fd, name.constData(), 0700) != 0 && errno != EEXIST) {
            ::close(fd);
            return -1;
        }
        const int next = ::openat(fd, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        if (next < 0)
            return -1;
        fd = next;
        walked += '/' + part;
        if (create && (walked == cacheRoot() || walked.startsWith(cacheRoot() + '/'))) {
            struct stat info {};
            if (::fstat(fd, &info) != 0 || info.st_uid != ::getuid() || ::fchmod(fd, 0700) != 0) {
                ::close(fd);
                return -1;
            }
        }
    }
    return fd;
}

QImage readPng(int fd, const QByteArray &name, int extent)
{
    if (fd < 0)
        return {};
    const int input = ::openat(fd, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (input < 0)
        return {};
    struct stat info {};
    QFile file;
    if (::fstat(input, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > 8 * 1024 * 1024
            || !file.open(input, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(input);
        return {};
    }
    QImageReader reader(&file, "png");
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > extent || size.height() > extent)
        return {};
    return reader.read();
}

bool matches(const QImage &image, const QString &uri, const struct stat &info)
{
    return !image.isNull() && image.text("Thumb::URI") == uri
        && image.text("Thumb::MTime") == QString::number(info.st_mtim.tv_sec)
        && (image.text("Thumb::Size").isEmpty()
            || image.text("Thumb::Size") == QString::number(info.st_size));
}

void publish(const QString &folder, const QByteArray &name, const QImage &image)
{
    const int fd = directory(folder, true);
    if (fd < 0)
        return;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || info.st_uid != ::getuid()
            || ::fchmod(fd, 0700) != 0
            || (::fstatat(fd, name.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0
                && !S_ISREG(info.st_mode))) {
        ::close(fd);
        return;
    }
    const QByteArray temporary = ".omafiles-" + QUuid::createUuid().toByteArray(QUuid::Id128);
    const int output = ::openat(fd, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    QFile file;
    if (output >= 0 && file.open(output, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        QBuffer encoded;
        encoded.open(QIODevice::WriteOnly);
        QImage pixels(image.constBits(), image.width(), image.height(), image.bytesPerLine(), image.format());
        pixels.setColorSpace(image.colorSpace());
        QImageWriter writer(&encoded, "png");
        bool saved = writer.write(pixels);
        if (saved) {
            const QByteArray png = encoded.data();
            saved = file.write(png.first(33)) == 33;
            // Qt compresses long text fields; the shared-cache contract requires tEXt.
            for (const QString &key : {QStringLiteral("Thumb::URI"), QStringLiteral("Thumb::MTime"),
                    QStringLiteral("Thumb::Size"), QStringLiteral("Software"), QStringLiteral("Thumb::Error")}) {
                if (image.text(key).isEmpty()) continue;
                const QByteArray chunk = "tEXt" + key.toLatin1() + '\0' + image.text(key).toLatin1();
                const quint32 length = qToBigEndian(quint32(chunk.size() - 4));
                const quint32 checksum = qToBigEndian(quint32(::crc32(0,
                    reinterpret_cast<const Bytef *>(chunk.constData()), chunk.size())));
                saved = saved && file.write(reinterpret_cast<const char *>(&length), 4) == 4
                    && file.write(chunk) == chunk.size()
                    && file.write(reinterpret_cast<const char *>(&checksum), 4) == 4;
            }
            saved = saved && file.write(png.mid(33)) == png.size() - 33 && file.flush();
        }
        file.close();
        if (saved)
            ::renameat(fd, temporary.constData(), fd, name.constData());
    } else if (output >= 0) {
        ::close(output);
    }
    ::unlinkat(fd, temporary.constData(), 0);
    ::close(fd);
}

QImage decode(const QString &path, int extent, bool *invalid = nullptr)
{
    if (invalid) *invalid = false;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid()) {
        if (invalid) *invalid = reader.error() == QImageReader::UnsupportedFormatError
            || reader.error() == QImageReader::InvalidDataError;
        return {};
    }
    if (qint64(size.width()) * size.height() > 64 * 1024 * 1024) return {};
    if (size.width() > extent || size.height() > extent)
        reader.setScaledSize(size.scaled(extent, extent, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    return image.width() > extent || image.height() > extent
        ? image.scaled(extent, extent, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
}

QStringList helperCommand(const QString &path, const QString &uri, const QString &output, int extent, ThumbnailWork *work)
{
    const QStringList roots = desktopDataDirs();
    QString mime;
    QSet<QString> seen;
    for (const QString &root : roots) {
        if (!root.startsWith('/'))
            continue;
        const QDir dir(root + "/thumbnailers");
        QStringList names;
        if (!readDirectory(dir.path(), &names, nullptr, nullptr)) continue;
        names.sort();
        for (const QString &name : std::as_const(names)) {
            if (!name.endsWith(".thumbnailer")) continue;
            if (seen.contains(name))
                continue;
            seen.insert(name);
            QFile file(dir.filePath(name));
            if (openToRead(&file, QIODevice::ReadOnly, nullptr) != PathState::Present) continue;
            QHash<QString, QString> entry;
            bool group = false;
            while (!file.atEnd()) {
                const QString line = QString::fromUtf8(file.readLine()).trimmed();
                if (line.startsWith('[')) {
                    group = line == "[Thumbnailer Entry]";
                    continue;
                }
                const qsizetype equals = line.indexOf('=');
                if (group && !line.startsWith('#') && equals > 0)
                    entry.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
            }
            const QStringList types = entry.value("MimeType").split(';', Qt::SkipEmptyParts);
            if (types.isEmpty())
                continue;
            if (mime.isEmpty()) {
                if (work) ++work->mimeLookups;
                mime = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchDefault).name();
            }
            if (!types.contains(mime))
                continue;
            const QString tryExec = entry.value("TryExec");
            if (!tryExec.isEmpty() && findExecutable(tryExec).isEmpty())
                continue;
            QStringList command = QProcess::splitCommand(entry.value("Exec"));
            bool valid = !command.isEmpty();
            for (QString &argument : command) {
                QString expanded;
                for (qsizetype i = 0; i < argument.size(); ++i) {
                    if (argument[i] != '%') {
                        expanded += argument[i];
                        continue;
                    }
                    const QChar code = ++i < argument.size() ? argument[i] : QChar();
                    if (code == 'i') expanded += path;
                    else if (code == 'u') expanded += uri;
                    else if (code == 'o') expanded += output;
                    else if (code == 's') expanded += QString::number(extent);
                    else if (code == '%') expanded += '%';
                    else valid = false;
                }
                argument = expanded;
            }
            const QString program = valid ? findExecutable(command.first()) : QString();
            if (!program.isEmpty()) {
                command.first() = program;
                return command;
            }
        }
    }
    return {};
}

bool runHelper(const QStringList &command, const ThumbnailCancellation &cancel)
{
    ThumbnailRunner custom;
    { QMutexLocker lock(&runnerMutex); custom = runner; }
    if (custom)
        return custom(command, cancel, helperBudget, helperCpu);
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    prepareHelper(process, 5, helperOutputLimit, helperCpu);
    process.start(command.first(), command.mid(1));
    QElapsedTimer elapsed;
    elapsed.start();
    while (!process.waitForFinished(25)) {
        if (cancel->load() || elapsed.elapsed() > 5000 || process.error() == QProcess::FailedToStart
                || helperResidentBytes(process.processId()) > helperBudget) {
            killHelper(process);
            process.waitForFinished();
            return {};
        }
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

struct Sources
{
    EntryWatcher *watcher = new EntryWatcher(qApp);
    QHash<QString, int> users;
    void update(QString &old, const QString &path)
    {
        if (old != path) {
            if (!old.isEmpty() && --users[old] == 0) {
                users.remove(old);
                watcher->removePath(old);
            }
            old = path;
            if (!path.isEmpty()) ++users[path];
        }
        if (!path.isEmpty())
            watcher->addPath(path);
    }
};
Sources &sources() { static Sources instance; return instance; }

class Queue : public QObject
{
public:
    struct Request { QPointer<Thumbnail> item; QString path; int extent; ThumbnailCancellation cancel; bool refresh; QSize renderSize; };
    Queue() : QObject(qApp), cpus(workerCpus()) {
        setObjectName("thumbnailQueue");
        pool.setMaxThreadCount(cpus.size());
        for (int cpu : cpus) {
            auto slot = std::make_unique<Slot>();
            slot->cpu = cpu;
            slot->watcher.setParent(this);
            connect(&slot->watcher, &QFutureWatcher<QImage>::finished, this, [this, active = slot.get()] {
                if (active->current.item)
                    active->current.item->deliver(active->current.cancel, active->watcher.result());
                active->current = {};
                active->busy = false;
                start();
            });
            activeSlots.push_back(std::move(slot));
        }
    }
    ~Queue() override {
        for (const auto &slot : activeSlots)
            if (slot->current.cancel) slot->current.cancel->store(true);
        pool.waitForDone();
    }
    void remove(Thumbnail *item) {
        pending.erase(std::remove_if(pending.begin(), pending.end(), [item](const Request &r) {
            return !r.item || r.item == item;
        }), pending.end());
    }
    bool add(const Request &request) {
        remove(request.item);
        if (pending.size() == 128)
            return false;
        pending.append(request);
        start();
        return true;
    }
private:
    struct Slot {
        QFutureWatcher<QImage> watcher;
        Request current;
        int cpu = 0;
        bool busy = false;
    };
    void start() {
        for (const auto &slot : activeSlots) {
            if (slot->busy) continue;
            while (!pending.isEmpty() && (!pending.first().item || pending.first().cancel->load()))
                pending.removeFirst();
            if (pending.isEmpty()) return;
            slot->busy = true;
            slot->current = pending.takeFirst();
            const auto request = slot->current;
            slot->watcher.setFuture(QtConcurrent::run(&pool, [request, cpu = slot->cpu, workers = cpus.size()] {
                if (request.cancel->load()) return QImage();
                helperCpu = workers > 1 ? cpu : -1;
                helperBudget = helperMemoryLimit / workers;
                const QImage image = thumbnailFor(request.path, request.extent, request.cancel, request.refresh);
                if (!request.renderSize.isEmpty()
                    && (image.width() > request.renderSize.width() || image.height() > request.renderSize.height()))
                    return image.scaled(request.renderSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                return image;
            }));
        }
    }
    const QList<int> cpus;
    QThreadPool pool;
    std::vector<std::unique_ptr<Slot>> activeSlots;
    QList<Request> pending;
};

Queue &queue()
{
    static QPointer<Queue> instance;
    if (!instance) instance = new Queue;
    return *instance;
}
}


ThumbnailRunner setThumbnailRunner(ThumbnailRunner next)
{
    QMutexLocker lock(&runnerMutex);
    return std::exchange(runner, std::move(next));
}

QImage thumbnailFor(const QString &path, int extent, const ThumbnailCancellation &cancel,
                    bool refresh, ThumbnailWork *work)
{
    if (work) *work = {};
    const QString clean = QDir::cleanPath(path);
    const QString root = cacheRoot();
    if (cancel->load() || !clean.startsWith('/') || clean == root || clean.startsWith(root + '/'))
        return {};
    struct stat before {};
    if (::stat(QFile::encodeName(clean).constData(), &before) != 0 || !S_ISREG(before.st_mode)
            || ::access(QFile::encodeName(clean).constData(), R_OK) != 0)
        return {};
    const QString canonical = realPath(clean, nullptr);
    if (canonical.isEmpty() || canonical == root || canonical.startsWith(root + '/')) return {};
    extent = extent > 128 ? 256 : 128;
    const QString uri = QString::fromUtf8(QUrl::fromLocalFile(clean).toEncoded());
    const QByteArray name = QCryptographicHash::hash(uri.toUtf8(), QCryptographicHash::Md5).toHex() + ".png";
    const QString tier = extent == 128 ? "normal" : "large";
    const QString failure = root + "/fail/omafiles-" + QStringLiteral(OMAFILES_VERSION);
    for (const QString &folder : QStringList{root + '/' + tier, extent == 128 ? root + "/large" : QString()}) {
        if (folder.isEmpty()) continue;
        const int fd = directory(folder, false);
        const QImage cached = readPng(fd, name, folder.endsWith("/normal") ? 128 : 256);
        if (fd >= 0) ::close(fd);
        if (!refresh && matches(cached, uri, before)) return cached;
    }
    const int failed = directory(failure, false);
    const QImage marker = readPng(failed, name, 1);
    if (failed >= 0) ::close(failed);
    if (!refresh && marker.text("Thumb::Error") == "invalid-data" && matches(marker, uri, before)) return {};
    if (cancel->load()) return {};
    bool invalid = false;
    QImage image;
    if (before.st_size > 0) {
        if (work) ++work->sourceDecodes;
        image = decode(clean, extent, &invalid);
    }
    if (image.isNull() && !cancel->load()) {
        QTemporaryDir scratch;
        if (!scratch.isValid()) return {};
        const QString output = scratch.filePath("thumbnail.png");
        const QStringList command = helperCommand(clean, uri, output, extent, work);
        if (command.isEmpty()) {
            if (before.st_size == 0
                || !QImageReader::supportedImageFormats().contains(QFileInfo(clean).suffix().toLower().toLatin1()))
                return {};
        } else {
            if (!runHelper(command, cancel)) return {};
            image = decode(output, extent, &invalid);
        }
    }
    struct stat after {};
    if (cancel->load() || ::stat(QFile::encodeName(clean).constData(), &after) != 0
            || before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec
            || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ino != after.st_ino || before.st_dev != after.st_dev
            || ::access(QFile::encodeName(clean).constData(), R_OK) != 0)
        return {};
    const bool ok = !image.isNull();
    if (!ok && !invalid) return {};
    if (!ok) {
        image = QImage(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        image.setText("Thumb::Error", "invalid-data");
    }
    image.setText("Thumb::URI", uri);
    image.setText("Thumb::MTime", QString::number(before.st_mtim.tv_sec));
    image.setText("Thumb::Size", QString::number(before.st_size));
    image.setText("Software", "omafiles-studio");
    publish(ok ? root + '/' + tier : failure, name, image);
    return ok ? image : QImage();
}

Thumbnail::Thumbnail(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setObjectName("thumbnail");
    m_delay.setSingleShot(true);
    m_delay.setInterval(0);
    const auto changed = [this] {
        m_refresh = true;
        restart();
        if (m_requested) m_delay.start(60);
        emit sourceChanged();
    };
    connect(sources().watcher, &EntryWatcher::fileChanged, this, [this, changed](const QString &path) {
        if (path == m_path) changed();
    });
    connect(sources().watcher, &EntryWatcher::directoryChanged, this, [this, changed](const QString &path) {
        if (path == m_watchedDirectory && !sources().watcher->contains(m_path)) changed();
    });
    connect(&m_delay, &QTimer::timeout, this, [this] {
        watch();
        m_renderSize = renderSize();
        if (!queue().add({this, m_path, m_extent, m_cancel, m_refresh, m_renderSize}))
            m_delay.start(120);
    });
}
Thumbnail::~Thumbnail()
{
    sources().update(m_watchedPath, {});
    sources().update(m_watchedDirectory, {});
    if (m_cancel) m_cancel->store(true);
    queue().remove(this);
}
void Thumbnail::watch()
{
    sources().update(m_watchedPath, m_path);
    sources().update(m_watchedDirectory, m_path.isEmpty() ? QString() : typedDirectory(m_path, nullptr));
}
void Thumbnail::setPath(const QString &path)
{
    if (m_path == path) return;
    m_path = path;
    m_refresh = false;
    watch();
    emit pathChanged();
    restart();
}
void Thumbnail::setExtent(int extent) { if (m_extent != extent) { m_extent = extent; emit extentChanged(); restart(); } }
void Thumbnail::setRequested(bool requested) { if (m_requested != requested) { m_requested = requested; emit requestedChanged(); restart(true); } }
QSize Thumbnail::renderSize() const
{
    if (width() <= 0 || height() <= 0) return {};
    const qreal ratio = window() ? window()->devicePixelRatio() : 1;
    const int tier = m_extent > 128 ? 256 : 128;
    return QSize(qCeil(width() * ratio), qCeil(height() * ratio)).boundedTo(QSize(tier, tier));
}
void Thumbnail::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size() && m_requested && renderSize() != m_renderSize)
        restart(true);
}
void Thumbnail::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickPaintedItem::itemChange(change, data);
    if ((change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged)
        && m_requested && renderSize() != m_renderSize)
        restart(true);
}
void Thumbnail::restart(bool retainImage)
{
    m_delay.stop();
    if (m_cancel) m_cancel->store(true);
    queue().remove(this);
    m_cancel = std::make_shared<std::atomic_bool>(false);
    if (!retainImage) {
        m_image = {};
        emit readyChanged();
        update();
    }
    if (m_requested && !m_path.isEmpty()) m_delay.start();
}
void Thumbnail::deliver(const ThumbnailCancellation &cancel, const QImage &image)
{
    if (cancel != m_cancel || cancel->load() || !m_requested) return;
    m_refresh = false;
    m_image = image;
    emit readyChanged();
    update();
}
void Thumbnail::paint(QPainter *painter)
{
    if (m_image.isNull()) return;
    const QSizeF size = QSizeF(m_image.size()).scaled(width(), height(), Qt::KeepAspectRatio);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(), size.height()), m_image);
}
}
