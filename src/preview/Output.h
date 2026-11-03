#pragma once

#include "Protocol.h"
#include <QtEndian>
#include <cerrno>
#include <unistd.h>

namespace omafiles {
// The file descriptor on which omafiles-preview hands the open file to omafiles-preview-codecs.
constexpr int previewFile = 3;

// Writes one length-prefixed packet to stdout.
inline bool send(const PreviewPacket &packet)
{
    const QByteArray bytes = previewPacket(packet);
    if (bytes.size() > previewPacketLimit) return false;
    QByteArray framed(4, '\0');
    qToBigEndian(quint32(bytes.size()), framed.data());
    framed += bytes;
    qsizetype written = 0;
    while (written < framed.size()) {
        const ssize_t count = ::write(STDOUT_FILENO, framed.constData() + written, framed.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        written += count;
    }
    return true;
}

inline int error(const QString &message) { send({"error", message}); return 1; }
}
