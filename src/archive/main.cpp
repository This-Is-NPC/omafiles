#include "ArchiveWorker.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <stdexcept>

namespace {
void send(const QJsonObject &message) {
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout) != size_t(bytes.size()) || std::fflush(stdout))
        std::exit(1);
}
}
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        QByteArray bytes;
        char buffer[65536];
        for (;;) {
            const size_t count = std::fread(buffer, 1, sizeof(buffer), stdin);
            bytes.append(buffer, qsizetype(count));
            if (bytes.size() > 1024 * 1024) throw std::runtime_error("Archive selection is too large.");
            if (count < sizeof(buffer)) {
                if (std::ferror(stdin)) throw std::runtime_error("Cannot read archive request.");
                if (std::feof(stdin)) break;
            }
        }
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) throw std::runtime_error("Invalid archive request.");
        const auto request = document.object();
        QStringList paths;
        for (const auto &path : request.value("paths").toArray()) paths.append(path.toString());
        const auto names = omafiles::runArchive(paths, request.value("destination").toString(),
            request.value("format").toString(), request.value("extracting").toBool(), request.value("stage").toString(),
            [](qint64 entries, qint64 processed) { send({{"entries", entries}, {"processed", processed}}); });
        for (const auto &name : names) send({{"entry", name}});
        send({{"ready", true}});
        return 0;
    } catch (const std::exception &error) {
        send({{"error", QString::fromUtf8(error.what())}});
        return 1;
    }
}
