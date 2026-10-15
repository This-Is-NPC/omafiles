#include "FileManager.h"
#include "Paths.h"
#include "Roots.h"
#include "Workspace.h"
#include "Devices.h"
#include "Portal.h"
#include "omafiles_version.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QTextStream>
#include <QUrl>
#include <QTimer>

int main(int argc, char **argv)
{
    const QString activationToken = omafiles::FileManager::launchToken();
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("omafiles-studio"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OMAFILES_VERSION));
    QGuiApplication::setQuitOnLastWindowClosed(true);

    // Refuse byte sequences that would decode into a different path.
    QStringList args;
    QString error;
    if (!omafiles::argumentsFromBytes(argc, argv, &args, &error)
        || !omafiles::environmentNameable(&error)) {
        QTextStream(stderr) << QStringLiteral("omafiles-studio: ") << error << Qt::endl;
        return 1;
    }

    if (args == QStringList{QStringLiteral("--portal")}) {
        QGuiApplication::setQuitOnLastWindowClosed(false);
        QQmlApplicationEngine engine;
        auto *portal = engine.singletonInstance<omafiles::Portal *>("omafiles", "Portal");
        engine.load(QUrl(QStringLiteral("qrc:/qml/FileChooser.qml")));
        if (engine.rootObjects().isEmpty() || !portal->start())
            return 1;
        QTimer idle;
        idle.setSingleShot(true);
        idle.setInterval(30000);
        QObject::connect(portal, &omafiles::Portal::currentChanged, &idle, [&] {
            if (portal->current()) idle.stop();
            else idle.start();
        });
        QObject::connect(&idle, &QTimer::timeout, &app, &QCoreApplication::quit);
        idle.start();
        return app.exec();
    }

    QList<omafiles::OpenLocation> locations;
    if (!omafiles::FileManager::resolve(args, omafiles::FileManager::Automatic,
                                       &locations, &error)) {
        QTextStream(stderr) << "omafiles-studio: " << error << Qt::endl;
        return 2;
    }

    const QString instance = QStringLiteral("org.omafiles.Studio");
    const QString objectPath = QStringLiteral("/org/freedesktop/FileManager1");
    const QString interface = QStringLiteral("org.freedesktop.FileManager1");
    auto bus = QDBusConnection::sessionBus();
    omafiles::FileManager manager;
    if (bus.isConnected()) {
        if (!bus.registerObject(objectPath, &manager, QDBusConnection::ExportAllSlots))
            return 1;
        if (!bus.registerService(instance)) {
            QStringList uris;
            for (const auto &location : locations)
                uris.append(QUrl::fromLocalFile(location.cursor.isEmpty()
                                                ? location.directory : location.cursor).toString(QUrl::FullyEncoded));
            QDBusInterface running(instance, objectPath, interface, bus);
            QDBusReply<QString> reply = running.call(QStringLiteral("Open"), uris,
                activationToken);
            if (!reply.isValid() || !reply.value().isEmpty()) {
                QTextStream(stderr) << "omafiles-studio: "
                    << (reply.isValid() ? reply.value() : reply.error().message()) << Qt::endl;
                return 1;
            }
            return 0;
        }
        bus.interface()->registerService(interface, QDBusConnectionInterface::QueueService);
    }

    // Read declared roots before any model decides whether a path is present.
    if (!omafiles::loadRoots(&error))
        QTextStream(stderr) << QStringLiteral("omafiles-studio: ") << error << Qt::endl;

    // QML types are registered by the build; the workspace is prepared before loading the view.
    QQmlApplicationEngine engine;
    QObject::connect(&app, &QGuiApplication::lastWindowClosed, &app, [] {
        QCoreApplication::exit(0);
    });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                     [](QObject *object, const QUrl &) {
                         if (!object)
                             QCoreApplication::exit(1);
                     },
                     Qt::QueuedConnection);
    auto *workspace = engine.singletonInstance<omafiles::Workspace *>("omafiles", "Workspace");
    manager.setWorkspace(workspace);
    manager.open(locations, true);
    if (!activationToken.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
    auto *devices = engine.singletonInstance<omafiles::Devices *>("omafiles", "Devices");
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QObject::connect(window, &QQuickWindow::frameSwapped,
                     devices, [devices] { devices->start(); }, Qt::SingleShotConnection);
    manager.setWindow(window);
    return app.exec();
}
