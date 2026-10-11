#include "Paths.h"
#include "Roots.h"
#include "Workspace.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QTextStream>
#include <QUrl>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("omafiles-studio"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OMAFILES_VERSION));
    QGuiApplication::setQuitOnLastWindowClosed(true);

    // The rule the command line keeps, for the same reason: a folder or a base
    // directory decoded into U+FFFD names some other folder.
    QStringList args;
    QString error;
    if (!omafiles::argumentsFromBytes(argc, argv, &args, &error)
        || !omafiles::environmentNameable(&error)) {
        QTextStream(stderr) << QStringLiteral("omafiles-studio: ") << error << Qt::endl;
        return 1;
    }

    // A positional folder, the way every other file manager takes one, so the
    // studio opens where the caller is and can be the target of a directory
    // open rather than only ever starting at home. Resolved and checked here
    // and not in QML: a path that does not name a folder is an exit code, and
    // a window that opened at home instead would report success for it.
    if (!args.isEmpty()) {
        QTextStream err(stderr);
        const QString folder = omafiles::canonicalPath(args.first(), &error);
        if (folder.isEmpty()) {
            err << QStringLiteral("omafiles-studio: ") << error << Qt::endl;
            return 2;
        }
        if (!QFileInfo(folder).isDir()) {
            err << QStringLiteral("omafiles-studio: ") << folder
                << QStringLiteral(" is not a folder") << Qt::endl;
            return 2;
        }
        omafiles::TabState::setStartPath(folder);
    }

    // Before the engine, for the reason the command line reads them before any
    // verb: every answer about a path depends on which roots are declared, and
    // a window that read them later would spend its first frame calling files
    // missing that are merely on a disk nobody has plugged in.
    if (!omafiles::loadRoots(&error))
        QTextStream(stderr) << QStringLiteral("omafiles-studio: ") << error << Qt::endl;

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
