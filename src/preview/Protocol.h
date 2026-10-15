#pragma once

#include <QDataStream>
#include <QImage>
#include <QIODevice>
#include <utility>

namespace omafiles {
constexpr quint32 previewPacketLimit = 8 * 1024 * 1024;
struct PreviewPacket {
    PreviewPacket(QString kind = {}, QString text = {}, bool playing = false,
                  bool loading = false, QImage image = {})
        : kind(std::move(kind)), text(std::move(text)), playing(playing), loading(loading), image(std::move(image)) {}
    QString kind;
    QString text;
    bool playing = false;
    bool loading = false;
    QImage image;
};

inline QByteArray previewPacket(const PreviewPacket &packet)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    const QImage image = packet.image.convertToFormat(QImage::Format_ARGB32);
    out << packet.kind << packet.text << packet.playing << packet.loading << image.width() << image.height()
        << QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    return bytes;
}

inline bool readPreviewPacket(const QByteArray &bytes, PreviewPacket *packet)
{
    QDataStream in(bytes);
    in.setVersion(QDataStream::Qt_6_0);
    int width = 0, height = 0;
    QByteArray pixels;
    in >> packet->kind >> packet->text >> packet->playing >> packet->loading >> width >> height >> pixels;
    if (in.status() != QDataStream::Ok || packet->text.size() > 128 * 1024
            || width < 0 || height < 0 || width > 1280 || height > 1280
            || qint64(width) * height * 4 != pixels.size()) return false;
    if (width && height)
        packet->image = QImage(reinterpret_cast<const uchar *>(pixels.constData()),
                               width, height, QImage::Format_ARGB32).copy();
    return true;
}
}
