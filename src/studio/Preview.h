#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {
QString setPreviewHelper(QString program);

class Preview : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool playing READ playing NOTIFY changed)
    Q_PROPERTY(bool hasImage READ hasImage NOTIFY changed)
public:
    explicit Preview(QQuickItem *parent = nullptr);
    ~Preview() override;
    QString path() const { return m_path; }
    QString kind() const { return m_kind; }
    QString text() const { return m_text; }
    bool loading() const { return m_loading; }
    bool playing() const { return m_playing; }
    bool hasImage() const { return !m_image.isNull(); }
    void setPath(const QString &path);
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void seek(int seconds);
    void paint(QPainter *painter) override;
signals:
    void pathChanged();
    void changed();
private:
    void start();
    void receive();
    void fail(const QString &message);
    QProcess *m_process;
    QTimer m_monitor;
    QElapsedTimer m_elapsed;
    QByteArray m_bytes;
    QString m_path, m_kind, m_text;
    QImage m_image;
    bool m_pending = false, m_loading = false, m_playing = false, m_received = false, m_failed = false;
};
}
