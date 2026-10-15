#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <QWheelEvent>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <time.h>

// Keep the observer out of application helpers and decoder subprocesses.
__attribute__((constructor)) static void isolateObserver() { unsetenv("LD_PRELOAD"); }
static QString directory;
static QJsonObject pending;
static QElapsedTimer elapsed;
static qint64 started;
static int serial = 0;
static bool firstFrame = false;

static QPointer<QQuickWindow> mainWindow;
static void frame(QQuickWindow *quick);
static void registerWindow() {
    if (mainWindow) return;
    QQuickWindow *best = nullptr;
    for (auto *candidate : QGuiApplication::topLevelWindows()) {
        auto *quick = qobject_cast<QQuickWindow *>(candidate);
        if (quick && quick->width() >= 400 && quick->height() >= 250 &&
            (!best || quick->width()*quick->height() > best->width()*best->height())) best = quick;
    }
    if (!best) return;
    mainWindow = best;
    best->resize(1180, 720);
    QObject::connect(best, &QQuickWindow::frameSwapped, best, [best]() { frame(best); }, Qt::DirectConnection);
    best->update();
}
static QQuickWindow *window() { return mainWindow && mainWindow->isVisible() ? mainWindow.data() : nullptr; }
static QQuickItem *listing(QQuickItem *item) {
    if (!item->isVisible()) return nullptr;
    QQuickItem *best = item->property("count").isValid() && item->property("contentY").isValid() ? item : nullptr;
    for (auto *child : item->childItems()) {
        auto *candidate = listing(child);
        if (candidate && (!best || candidate->width() * candidate->height() > best->width() * best->height())) best = candidate;
    }
    return best;
}
static bool labelVisible(QQuickItem *item, const QString &text, const QRectF &viewport) {
    if (!item->isVisible()) return false;
    if (item->property("text").toString() == text && viewport.contains(item->mapToScene(QPointF(item->width()/2, item->height()/2)))) return true;
    for (auto *child : item->childItems()) if (labelVisible(child, text, viewport)) return true;
    return false;
}
static bool mediaReady(QQuickItem *item, bool preview, QQuickWindow *quick) {
    if (!item->isVisible()) return false;
    const QRectF viewport(0, 0, quick->width(), quick->height());
    if (viewport.contains(item->mapToScene(QPointF(item->width()/2, item->height()/2)))) {
        if (preview && item->inherits("omafiles::Preview") && item->property("hasImage").toBool() && !item->property("loading").toBool()) return true;
        if (!preview && item->inherits("omafiles::Thumbnail") && item->property("extent").toInt() == 256 && item->property("ready").toBool()) return true;
        bool insidePreview = false;
        for (auto *parent = item->parentItem(); parent; parent = parent->parentItem())
            if (QString::fromLatin1(parent->metaObject()->className()).contains("Preview")) insidePreview = true;
        if (item->inherits("QQuickImage") && item->property("status").toInt() == 1 && insidePreview == preview) {
            const auto source = item->property("source").toUrl().toString();
            if (!source.isEmpty() && !source.startsWith("qrc:") && (source.contains("image-") || source.contains("thumbnail"))) return true;
        }
    }
    for (auto *child : item->childItems()) if (mediaReady(child, preview, quick)) return true;
    return false;
}
static void report(QJsonObject event) {
    event["elapsed_ms"] = elapsed.nsecsElapsed()/1e6;
    const auto data = QJsonDocument(event).toJson(QJsonDocument::Compact);
    fprintf(stderr, "BENCH %s\n", data.constData()); fflush(stderr);
}
static void key(QQuickWindow *quick, const QString &name) {
    const auto sequence = QKeySequence::fromString(name);
    if (!sequence.count()) return;
    const auto combination = sequence[0];
    const int code = combination.key();
    const auto modifiers = combination.keyboardModifiers();
    QString text;
    if (code >= 32 && code < 127) text = modifiers.testFlag(Qt::ShiftModifier) ? QString(QChar(code)) : QString(QChar(code).toLower());
    QKeyEvent down(QEvent::KeyPress, code, modifiers, text), up(QEvent::KeyRelease, code, modifiers, text);
    QCoreApplication::sendEvent(quick, &down); QCoreApplication::sendEvent(quick, &up);
}
static void receive() {
    registerWindow();
    QFile file(directory + "/command.json");
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto command = QJsonDocument::fromJson(file.readAll()).object();
    const int next = command["serial"].toInt();
    auto *quick = window();
    if (!quick || next <= serial) return;
    serial = next; pending = command; started = elapsed.nsecsElapsed();
    report({{"event", "started"}, {"serial", serial}, {"operation", command["operation"]}});
    if (command.contains("key")) key(quick, command["key"].toString());
    if (command.contains("text")) for (const auto character : command["text"].toString()) {
        QKeyEvent down(QEvent::KeyPress, 0, Qt::NoModifier, QString(character)), up(QEvent::KeyRelease, 0, Qt::NoModifier, QString(character));
        QCoreApplication::sendEvent(quick, &down); QCoreApplication::sendEvent(quick, &up);
    }
    if (command["operation"] == "wheel") {
        const QPointF point(quick->width() * .65, quick->height() * .5);
        QWheelEvent event(point, quick->mapToGlobal(point.toPoint()), QPoint(), QPoint(0, -120000000), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(quick, &event);
    }
    quick->update();
}
static void frame(QQuickWindow *quick) {
    if (!firstFrame) {
        timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        report({{"event", "first_frame"}, {"ms", (now.tv_sec + now.tv_nsec/1e9 - qEnvironmentVariable("BENCH_START").toDouble()) * 1000}});
        report({{"event", "application"}, {"version", QCoreApplication::applicationVersion()}, {"name", QCoreApplication::applicationName()}, {"viewport_width", quick->width()}, {"viewport_height", quick->height()}});
        firstFrame = true;
    }
    if (pending.isEmpty()) return;
    auto *view = listing(quick->contentItem());
    const auto operation = pending["operation"].toString();
    bool ready = true;
    if (pending.contains("expected_count")) ready = view && view->property("count").toInt() == pending["expected_count"].toInt();
    if (pending.contains("label")) {
        const QRectF viewport = view ? QRectF(view->mapToScene(QPointF()), QSizeF(view->width(), view->height())) : QRectF();
        ready = ready && view && labelVisible(view, pending["label"].toString(), viewport);
    }
    if (operation == "wheel") ready = ready && view && view->property("contentY").toDouble() >= view->property("contentHeight").toDouble() - view->height() - 3;
    if (operation == "thumbnail" || operation == "preview") ready = mediaReady(quick->contentItem(), operation == "preview", quick);
    if (ready) {
        report({{"event", "complete"}, {"serial", serial}, {"operation", operation}, {"ms", (elapsed.nsecsElapsed()-started)/1e6}, {"visible_count", view ? view->property("count").toInt() : -1}});
        pending = {};
    }
}
class StartupWindowObserver : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show && qobject_cast<QQuickWindow *>(object)) {
            registerWindow();
            if (mainWindow) {
                QCoreApplication::instance()->removeEventFilter(this);
                receive();
            }
        }
        return false;
    }
};
static QFileSystemWatcher *observeCommands() {
    auto *watcher = new QFileSystemWatcher(QCoreApplication::instance());
    if (!watcher->addPath(directory)) {
        report({{"event", "error"}, {"reason", "Cannot watch benchmark command directory"}});
        delete watcher;
        return nullptr;
    }
    // The session atomically replaces command.json, so watch its parent directory.
    QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, watcher, []() { receive(); });
    registerWindow();
    if (!mainWindow) {
        auto *startup = new StartupWindowObserver(watcher);
        QCoreApplication::instance()->installEventFilter(startup);
    }
    receive();
    return watcher;
}
extern "C" int _ZN16QCoreApplication4execEv() {
    directory = qEnvironmentVariable("BENCH_DIRECTORY"); elapsed.start();
    if (!observeCommands()) return 2;
    const auto real = reinterpret_cast<int (*)()>(dlsym(RTLD_NEXT, "_ZN16QCoreApplication4execEv"));
    return real();
}
