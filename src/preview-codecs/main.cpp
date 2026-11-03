#include "../preview/Output.h"
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMediaPlayer>
#include <QPainter>
#include <QPdfDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <archive.h>
#include <archive_entry.h>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

using namespace omafiles;

// Previews the file omafiles-preview left open on previewFile as a PDF, audio, video or archive.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 3) return error("Preview needs one local file.");
    const QString kind = QString::fromLatin1(argv[1]);
    const QString path = QFile::decodeName(argv[2]);
    struct stat info {};
    if (::fstat(previewFile, &info) != 0 || ::lseek(previewFile, 0, SEEK_SET) != 0)
        return error("Cannot inspect this file: " + QString::fromLocal8Bit(std::strerror(errno)));
    QFile file;
    if (!file.open(previewFile, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
        return error("Cannot read this file: " + file.errorString());
    const auto within = [&](qint64 limit) { return info.st_size <= limit; };
    if (kind == "pdf") {
        if (!within(16 * 1024 * 1024)) return error("PDF preview is limited to 16 MiB.");
        QPdfDocument document;
        document.load(&file);
        if (document.status() != QPdfDocument::Status::Ready) {
            switch (document.error()) {
            case QPdfDocument::Error::IncorrectPassword: return error("This PDF requires a password.");
            case QPdfDocument::Error::UnsupportedSecurityScheme: return error("This PDF uses an unsupported security scheme.");
            case QPdfDocument::Error::InvalidFileFormat: return error("Cannot read this PDF: invalid PDF data.");
            case QPdfDocument::Error::FileNotFound: return error("Cannot read this PDF: file not found.");
            case QPdfDocument::Error::DataNotYetAvailable: return error("Cannot read this PDF: data is not yet available.");
            default: return error("Cannot read this PDF: the PDF decoder reported an unknown error.");
            }
        }
        if (document.pageCount() < 1) return error("This PDF has no pages to preview.");
        const QSize size = document.pagePointSize(0).toSize().scaled(1280, 1280, Qt::KeepAspectRatio);
        QImage image = document.render(0, size);
        if (image.isNull()) return error("Cannot render the first PDF page.");
        {
            QPainter painter(&image);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            painter.fillRect(image.rect(), Qt::white);
        }
        send({"pdf", QStringLiteral("Page 1 of %1").arg(document.pageCount()), false, false, image});
        return 0;
    }
    if (kind == "audio" || kind == "video") {
        send({kind, "Press P or Play to start. Sound is enabled only during explicit playback."});
        QMediaPlayer *player = nullptr;
        QAudioOutput *audio = nullptr;
        QVideoSink *sink = nullptr;
        QImage frame;
        QElapsedTimer lastFrame;
        lastFrame.start();
        const auto state = [&] {
            if (!player) return;
            const auto clock = [](qint64 ms) { return QStringLiteral("%1:%2").arg(ms / 60000)
                .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')); };
            const bool loading = player->mediaStatus() == QMediaPlayer::LoadingMedia;
            const QString caption = player->error() != QMediaPlayer::NoError ? player->errorString()
                : QStringLiteral("%1 · %2 / %3").arg(player->isPlaying() ? "Playing" : "Paused",
                                                      clock(player->position()), clock(player->duration()));
            if (!send({kind, caption, player->isPlaying(), loading, frame})) app.quit();
            frame = {};
        };
        const auto videoFrame = [&](const QVideoFrame &video) {
            if (!video.isValid() || lastFrame.elapsed() < 100) return;
            if (qint64(video.width()) * video.height() > 4096LL * 2160) {
                error("Video exceeds the 4096 × 2160 preview limit."); app.quit(); return;
            }
            frame = video.toImage().scaled(1280, 1280, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            lastFrame.restart(); state();
        };
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QByteArray commands;
        QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
            char buffer[256];
            const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0) { app.quit(); return; }
            commands.append(buffer, count);
            if (commands.size() > 1024) { app.quit(); return; }
            int newline;
            while ((newline = commands.indexOf('\n')) >= 0) {
                const QByteArray command = commands.left(newline);
                commands.remove(0, newline + 1);
                if (!player && command == "toggle") {
                    player = new QMediaPlayer(&app);
                    audio = new QAudioOutput(&app);
                    audio->setVolume(0.5);
                    player->setAudioOutput(audio);
                    if (kind == "video") {
                        sink = new QVideoSink(&app);
                        QObject::connect(sink, &QVideoSink::videoFrameChanged, &app, videoFrame);
                        player->setVideoSink(sink);
                    }
                    QObject::connect(player, &QMediaPlayer::playbackStateChanged, &app, state);
                    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, &app, state);
                    QObject::connect(player, &QMediaPlayer::errorOccurred, &app, state);
                    player->setSource(QUrl::fromLocalFile(path));
                }
                if (!player) continue;
                if (command == "pause") player->pause();
                else if (command == "toggle") {
                    if (player->isPlaying()) player->pause(); else player->play();
                } else if (command.startsWith("seek ")) {
                    const qint64 next = player->position() + qBound(-30, command.mid(5).toInt(), 30) * 1000;
                    player->setPosition(qBound(0LL, next, player->duration()));
                }
                state();
            }
        });
        QTimer ticker;
        ticker.setInterval(500);
        QObject::connect(&ticker, &QTimer::timeout, &app, state);
        ticker.start();
        return app.exec();
    }
    if (!within(64 * 1024 * 1024)) return error("Archive probing is limited to 64 MiB source files.");
    archive *reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    if (archive_read_open_fd(reader, previewFile, 16384) != ARCHIVE_OK) {
        const QString reason = QString::fromUtf8(archive_error_string(reader));
        archive_read_free(reader); return error("Cannot read this file as an archive: " + reason);
    }
    QString listing;
    archive_entry *entry = nullptr;
    int result = ARCHIVE_OK, count = 0;
    bool truncated = false;
    while ((result = archive_read_next_header(reader, &entry)) == ARCHIVE_OK) {
        if (count == 500 || listing.size() >= 60 * 1024) { truncated = true; break; }
        const char *rawName = archive_entry_pathname_utf8(entry);
        const QString name = QString::fromUtf8(rawName ? rawName : "[Unrepresentable name]");
        if (name.size() > 4096) truncated = true;
        listing += name.left(4096) + '\n';
        ++count;
        result = archive_read_data_skip(reader);
        if (result < ARCHIVE_OK) break;
    }
    const QString detail = result < ARCHIVE_OK ? QString::fromUtf8(archive_error_string(reader)) : QString();
    archive_read_free(reader);
    if (!detail.isEmpty()) return error("Cannot list this archive: " + detail);
    if (truncated) listing += "[Archive listing truncated]\n";
    send({"archive", listing.isEmpty() ? "Empty archive" : listing});
    return 0;
}
