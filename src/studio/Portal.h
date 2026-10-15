#pragma once
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QObject>
#include <QRegularExpression>
#include <QVariantList>
#include <QWindow>
#include <QtQmlIntegration/qqmlintegration.h>
#include <atomic>
#include <memory>
namespace omafiles {
class Portal;
class PortalRequest : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QString kind MEMBER kind CONSTANT)
    Q_PROPERTY(QString title MEMBER title CONSTANT)
    Q_PROPERTY(QString appId MEMBER appId CONSTANT)
    Q_PROPERTY(QString parentWindow MEMBER parentWindow CONSTANT)
    Q_PROPERTY(QString acceptLabel MEMBER acceptLabel CONSTANT)
    Q_PROPERTY(bool modal MEMBER modal CONSTANT)
    Q_PROPERTY(bool multiple MEMBER multiple CONSTANT)
    Q_PROPERTY(bool directory MEMBER directory CONSTANT)
    Q_PROPERTY(QString currentFolder MEMBER currentFolder CONSTANT)
    Q_PROPERTY(QString currentFile MEMBER currentFile CONSTANT)
    Q_PROPERTY(QString currentName MEMBER currentName CONSTANT)
    Q_PROPERTY(QStringList files MEMBER files CONSTANT)
    Q_PROPERTY(QVariantList filters MEMBER filters CONSTANT)
    Q_PROPERTY(QVariantList choices READ choiceList CONSTANT)
    Q_PROPERTY(QVariantMap choiceValues READ choiceValues NOTIFY changed)
    Q_PROPERTY(int selectedFilter READ selectedFilter WRITE setSelectedFilter NOTIFY changed)
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
    Q_PROPERTY(QStringList overwritePaths MEMBER overwritePaths NOTIFY changed)
public:
    QString kind, title, appId, parentWindow, acceptLabel, currentFolder, currentFile, currentName;
    QStringList files, overwritePaths;
    QVariantList filters;
    bool modal = true, multiple = false, directory = false, busy = false;
    QString error;
    QVariantList choiceList() const;
    QVariantMap choiceValues() const;
    int selectedFilter() const
    {
        return m_selectedFilter;
    }
    void setSelectedFilter(int index);
    Q_INVOKABLE void setChoice(const QString& id, const QString& value);
    Q_INVOKABLE void finish(const QStringList& paths);
    Q_INVOKABLE void confirmOverwrite(bool accept);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool matches(const QString& filename) const;
    Q_INVOKABLE void attachParent(QWindow* window);
signals:
    void changed();

private:
    friend class Portal;
    explicit PortalRequest(Portal* portal);
    bool parse(const QVariantMap& options, QString* reason);
    void prepareFilters();
    Portal* m_portal;
    QVariantList m_choices;
    int m_selectedFilter = -1;
    QDBusMessage m_reply;
    QString m_sender;
    QString m_handle;
    QStringList m_pending;
    bool m_completed = false;
    bool m_filtersReady = false;
    QList<QList<QRegularExpression>> m_patterns;
    std::shared_ptr<std::atomic_bool> m_cancelled = std::make_shared<std::atomic_bool>(false);
};
class Portal : public QDBusVirtualObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(omafiles::PortalRequest* current READ current NOTIFY currentChanged)
public:
    explicit Portal(QObject* parent = nullptr);
    ~Portal() override;
    bool start(const QDBusConnection& connection = QDBusConnection::sessionBus());
    PortalRequest* current() const
    {
        return m_requests.isEmpty() ? nullptr : m_requests.first();
    }
    QString introspect(const QString& path) const override;
    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override;
signals:
    void currentChanged();
private slots:
    void ownerChanged(const QString& name, const QString& oldOwner, const QString& newOwner);

private:
    friend class PortalRequest;
    void complete(PortalRequest* request, uint response, const QVariantMap& results = { });
    QList<PortalRequest*> m_requests;
    QDBusConnection m_connection;
    bool m_started = false;
};
}
