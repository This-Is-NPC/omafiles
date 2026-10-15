#include <QtTest>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <sys/inotify.h>
#include <unistd.h>
#include "probe.cpp"

class CommandReadWatch {
    int descriptor;
public:
    explicit CommandReadWatch(const QString &path) : descriptor(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
        if (descriptor >= 0 && inotify_add_watch(descriptor, QFile::encodeName(path), IN_OPEN | IN_ACCESS) < 0) {
            close(descriptor);
            descriptor = -1;
        }
    }
    ~CommandReadWatch() { if (descriptor >= 0) close(descriptor); }
    bool valid() const { return descriptor >= 0; }
    bool untouched() const {
        char buffer[4096];
        errno = 0;
        return read(descriptor, buffer, sizeof buffer) == -1 && errno == EAGAIN;
    }
    void discard() const {
        char buffer[4096];
        while (read(descriptor, buffer, sizeof buffer) > 0) {}
    }
};

class TestBenchmarkProbe : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QPointer<QFileSystemWatcher> observer;
    void writeCommand(int value) {
        QSaveFile output(directory + "/command.json");
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write(QJsonDocument(QJsonObject{{"serial", value}, {"operation", "listing"}}).toJson());
        QVERIFY(output.commit());
    }
    void show(QQuickWindow &quick) {
        quick.resize(1180, 720);
        quick.show();
        QTRY_VERIFY_WITH_TIMEOUT(quick.isVisible(), 30000);
    }
private slots:
    void init() {
        QVERIFY(temporary.isValid());
        directory = temporary.path();
        QFile::remove(directory + "/command.json");
        mainWindow = nullptr;
        pending = {};
        serial = 0;
        firstFrame = false;
        elapsed.start();
    }
    void cleanup() { delete observer; }
    void readsExistingCommandsAfterInstallingTheWatch() {
        QQuickWindow quick;
        show(quick);
        writeCommand(1);
        observer = observeCommands();
        QVERIFY(observer);
        QCOMPARE(serial, 1);
        QCOMPARE(pending["operation"].toString(), QString("listing"));
    }
    void receivesRepeatedAtomicReplacements() {
        QQuickWindow quick;
        show(quick);
        writeCommand(0);
        observer = observeCommands();
        QVERIFY(observer);
        for (int value = 1; value <= 5; ++value) {
            writeCommand(value);
            QTRY_COMPARE_WITH_TIMEOUT(serial, value, 30000);
            frame(&quick);
            QVERIFY(pending.isEmpty());
        }
        QSignalSpy changed(observer, &QFileSystemWatcher::directoryChanged);
        writeCommand(3);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 30000);
        QCOMPARE(serial, 5);
        QVERIFY(pending.isEmpty());
    }
    void receivesCommandsBeforeTheWindowExists() {
        observer = observeCommands();
        QVERIFY(observer);
        writeCommand(1);
        QCoreApplication::processEvents();
        QCOMPARE(serial, 0);
        QQuickWindow small;
        small.resize(100, 100);
        small.show();
        QCOMPARE(serial, 0);
        QQuickWindow quick;
        show(quick);
        QTRY_COMPARE_WITH_TIMEOUT(mainWindow.data(), &quick, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(serial, 1, 30000);
        CommandReadWatch reads(directory + "/command.json");
        QVERIFY(reads.valid());
        QQuickWindow another;
        show(another);
        QCoreApplication::processEvents();
        QVERIFY(reads.untouched());
    }
    void doesNotPollOrReadCommandsWithoutDirectoryChanges() {
        QQuickWindow quick;
        show(quick);
        writeCommand(1);
        observer = observeCommands();
        QVERIFY(observer);
        for (auto *timer : qApp->findChildren<QTimer *>()) QVERIFY(!timer->isActive());
        CommandReadWatch reads(directory + "/command.json");
        QVERIFY(reads.valid());
        QFile log(directory + "/gui.log");
        QVERIFY(log.open(QIODevice::WriteOnly));
        QCoreApplication::processEvents();
        reads.discard();
        for (int turn = 0; turn < 10; ++turn) {
            log.write("application log\n");
            log.flush();
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
            QVERIFY(reads.untouched());
        }
    }
    void preservesFrameCompletionConditions() {
        QQuickWindow quick;
        show(quick);
        auto *view = quick.contentItem();
        view->setWidth(1180);
        view->setHeight(720);
        view->setProperty("count", 1);
        view->setProperty("contentY", 0);
        view->setProperty("contentHeight", 2000);
        QQuickItem label(view);
        label.setWidth(20);
        label.setHeight(20);
        label.setProperty("text", "waiting");
        observer = observeCommands();
        QVERIFY(observer);
        pending = {{"operation", "listing"}, {"expected_count", 2}, {"label", "ready"}};
        frame(&quick);
        QVERIFY(!pending.isEmpty());
        view->setProperty("count", 2);
        frame(&quick);
        QVERIFY(!pending.isEmpty());
        label.setProperty("text", "ready");
        frame(&quick);
        QVERIFY(pending.isEmpty());
        pending = {{"operation", "wheel"}};
        frame(&quick);
        QVERIFY(!pending.isEmpty());
        view->setProperty("contentY", 1280);
        frame(&quick);
        QVERIFY(pending.isEmpty());
    }
    void refusesAnUnavailableCommandDirectory() {
        directory += "/missing";
        observer = observeCommands();
        QVERIFY(!observer);
        for (auto *timer : qApp->findChildren<QTimer *>()) QVERIFY(!timer->isActive());
    }
};
QTEST_MAIN(TestBenchmarkProbe)
#include "tst_benchmark_probe.moc"
