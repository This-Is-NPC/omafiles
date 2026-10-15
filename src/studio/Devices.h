#pragma once

#include <QObject>
#include <QProcess>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {
class Devices : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit Devices(QObject *parent = nullptr);
    ~Devices() override;
    QVariantList entries() const { return m_entries; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    void start(const QString &program = {});
    Q_INVOKABLE void act(const QString &id, const QString &action);
    Q_INVOKABLE void refresh();
signals:
    void changed();
    void storageChanged();
    void openRequested(QString path);
    void notice(QString message);
private:
    void receive();
    void fail(const QString &reason);
    QProcess *m_process;
    QVariantList m_entries;
    QByteArray m_input;
    QString m_status, m_pending, m_action;
    bool m_busy = false, m_failed = false;
};
}
