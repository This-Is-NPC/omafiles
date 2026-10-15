#pragma once

#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <functional>

namespace omafiles::Trash {

struct Location {
    QString path;
    QString top;
};

struct Item {
    QString path;
    QString info;
    QString original;
    QString deleted;
    QString error;
    Location location;
    QByteArray metadata;
    QByteArray signature;
    QByteArray metadataSignature;
    quint64 device = 0;
    quint64 inode = 0;
    quint64 fileDevice = 0;
    quint64 fileInode = 0;
};

struct Listing {
    QVector<Item> items;
    QStringList errors;
    QStringList watches;
};

using MountProvider = std::function<QStringList()>;
void setMountProvider(MountProvider provider);
QStringList mounts();

Listing list(const QStringList &mounts,
             const std::shared_ptr<std::atomic_bool> &cancel = {});
QString restore(const Item &item);
QString erase(const Item &item);

} // namespace omafiles::Trash
