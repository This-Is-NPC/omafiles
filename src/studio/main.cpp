#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QUrl>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("omafiles-studio"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OMAFILES_VERSION));
    QGuiApplication::setQuitOnLastWindowClosed(true);

    // Nothing is registered or injected here. `GraphItem` and `FolderModel`
    // carry QML_ELEMENT and `Theme`, `IndexModel` and `Workspace` carry
    // QML_SINGLETON, so the build registers all five and the engine makes the
    // three singletons on first use -- rather than this and the test harness
    // each keeping an identical block of registrations in step by memory.
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
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
