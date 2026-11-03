#include "Output.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeDatabase>
#include <QStringDecoder>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

using namespace omafiles;
namespace {
int imageError(const QImageReader &reader)
{
    const QString reason = reader.error() == QImageReader::UnknownError
        ? QStringLiteral("incomplete or unreadable image data.") : reader.errorString();
    return error("Cannot read this image: " + reason);
}

// Replaces this process with omafiles-preview-codecs, which reads the open file on previewFile.
int codecs(const char *kind, int fd, char *path)
{
    if (fd == previewFile ? ::fcntl(fd, F_SETFD, 0) != 0 : ::dup2(fd, previewFile) != previewFile)
        return error("Cannot hand this file to the preview helper: " + QString::fromLocal8Bit(std::strerror(errno)));
    const QByteArray program = QFile::encodeName(QFileInfo(QFileInfo(QStringLiteral("/proc/self/exe")).symLinkTarget())
                                                     .dir().filePath(QStringLiteral("omafiles-preview-codecs")));
    char *arguments[] = {const_cast<char *>(program.constData()), const_cast<char *>(kind), path, nullptr};
    ::execv(program.constData(), arguments);
    return error("Preview helper could not start: " + QString::fromLocal8Bit(std::strerror(errno)));
}

QString cappedText(const QString &text, bool *truncated)
{
    QString output;
    int lines = 0, column = 0;
    for (const QChar character : text) {
        if (lines == 1000) { *truncated = true; break; }
        if (character == '\n') { ++lines; column = 0; output += character; }
        else if (++column <= 4096) output += character;
        else *truncated = true;
    }
    if (*truncated) output += "\n[Preview truncated]\n";
    return output;
}
}

int main(int argc, char **argv)
{
    if (argc != 2) return error("Preview needs one local file.");
    const int fd = ::open(argv[1], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat info {};
    if (fd < 0) return error("Cannot read this file: " + QString::fromLocal8Bit(std::strerror(errno)));
    if (::fstat(fd, &info) != 0) {
        const QString reason = QString::fromLocal8Bit(std::strerror(errno));
        ::close(fd); return error("Cannot inspect this file: " + reason);
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd); return error("Preview supports regular files only.");
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd); return error("Cannot read this file: " + file.errorString());
    }
    const QByteArray prefix = file.peek(64 * 1024);
    if (file.error() != QFileDevice::NoError) return error("Cannot read this file: " + file.errorString());
    const QMimeType type = QMimeDatabase().mimeTypeForData(prefix);
    const QString mime = type.name();
    if (mime == "application/pdf") return codecs("pdf", fd, argv[1]);
    if (mime.startsWith("image/")) {
        if (info.st_size > 64 * 1024 * 1024) return error("Image preview is limited to 64 MiB source files.");
        QGuiApplication app(argc, argv);
        QImageReader::setAllocationLimit(32);
        QImageReader reader(&file, type.preferredSuffix().toLatin1());
        reader.setAutoTransform(true);
        const QSize original = reader.size();
        if (!original.isValid()) return imageError(reader);
        if (qint64(original.width()) * original.height() > 64 * 1024 * 1024)
            return error("Image dimensions exceed the 64-megapixel preview limit.");
        if (type.inherits("image/svg+xml")
            || original.width() > 1280 || original.height() > 1280)
            reader.setScaledSize(original.scaled(1280, 1280, Qt::KeepAspectRatio));
        const QImage image = reader.read();
        if (image.isNull()) return imageError(reader);
        send({"image", QStringLiteral("%1 × %2").arg(original.width()).arg(original.height()), false, false, image});
        return 0;
    }
    if ((mime.startsWith("audio/") || mime.startsWith("video/"))
            && !mime.contains("mpegurl") && !mime.contains("scpls"))
        return codecs(mime.startsWith("video/") ? "video" : "audio", fd, argv[1]);
    if (type.inherits("text/plain") || mime == "application/json" || mime.endsWith("+xml")
            || mime == "application/xml" || prefix.isEmpty()) {
        file.seek(0);
        const QByteArray bytes = file.read(64 * 1024);
        if (file.error() != QFileDevice::NoError) return error("Cannot read this file: " + file.errorString());
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder.decode(bytes);
        if (decoder.hasError() || bytes.contains('\0')) return error("This is not UTF-8 text.");
        bool truncated = info.st_size > bytes.size();
        send({"text", cappedText(text, &truncated)});
        return 0;
    }
    return codecs("archive", fd, argv[1]);
}
