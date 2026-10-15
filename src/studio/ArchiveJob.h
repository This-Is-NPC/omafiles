#pragma once

#include <QObject>
#include <QStringList>
#include <memory>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {
class ArchiveJob : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool canceled READ canceled NOTIFY changed)
    Q_PROPERTY(QString progress READ progress NOTIFY changed)
    Q_PROPERTY(qint64 processed READ processed NOTIFY changed)
public:
    explicit ArchiveJob(QObject *parent = nullptr);
    ~ArchiveJob() override;
    bool busy() const;
    bool canceled() const;
    QString progress() const;
    qint64 processed() const;
    Q_INVOKABLE void compress(const QStringList &paths, const QString &destination, const QString &format);
    Q_INVOKABLE void extract(const QString &path, const QString &destination);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void complete(bool ok, const QString &message);
    void updateProgress(const QString &message);
signals:
    void changed();
    void ready(const QStringList &stagedPaths, const QString &destination, const QString &label);
    void finished(bool ok, const QString &message);
private:
    struct Private;
    std::unique_ptr<Private> d;
    void start(const QStringList &paths, const QString &destination, const QString &format, bool extracting);
};
}
