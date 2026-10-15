#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>
#include <atomic>
#include <functional>
#include <memory>

namespace omafiles {

using ThumbnailCancellation = std::shared_ptr<std::atomic_bool>;
using ThumbnailRunner = std::function<bool(const QStringList &, const ThumbnailCancellation &, qint64, int)>;
ThumbnailRunner setThumbnailRunner(ThumbnailRunner runner);
struct ThumbnailWork { int sourceDecodes = 0; int mimeLookups = 0; };
QImage thumbnailFor(const QString &path, int extent, const ThumbnailCancellation &cancel,
                    bool refresh = false, ThumbnailWork *work = nullptr);

class Thumbnail : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(int extent READ extent WRITE setExtent NOTIFY extentChanged)
    Q_PROPERTY(bool requested READ requested WRITE setRequested NOTIFY requestedChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
public:
    explicit Thumbnail(QQuickItem *parent = nullptr);
    ~Thumbnail() override;
    QString path() const { return m_path; }
    int extent() const { return m_extent; }
    bool requested() const { return m_requested; }
    bool ready() const { return !m_image.isNull(); }
    QSize imageSize() const { return m_image.size(); }
    void setPath(const QString &path);
    void setExtent(int extent);
    void setRequested(bool requested);
    void deliver(const ThumbnailCancellation &cancel, const QImage &image);
    void paint(QPainter *painter) override;
signals:
    void sourceChanged();
    void pathChanged();
    void extentChanged();
    void requestedChanged();
    void readyChanged();
protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;
private:
    QSize renderSize() const;
    void restart(bool retainImage = false);
    void watch();
    QString m_watchedPath;
    QString m_watchedDirectory;
    bool m_refresh = false;
    QString m_path;
    int m_extent = 128;
    bool m_requested = false;
    QImage m_image;
    QSize m_renderSize;
    QTimer m_delay;
    ThumbnailCancellation m_cancel;
};
}
