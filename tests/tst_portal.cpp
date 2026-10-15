#include "Portal.h"
#include <QDBusArgument>
#include <QDBusConnectionInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
using namespace omafiles;
struct TestRule {
    uint type;
    QString pattern;
};
struct TestFilter {
    QString name;
    QList<TestRule> rules;
};
struct TestOption {
    QString id, label;
};
struct TestChoice {
    QString id, label;
    QList<TestOption> options;
    QString selected;
};
Q_DECLARE_METATYPE(TestRule)
Q_DECLARE_METATYPE(TestFilter)
Q_DECLARE_METATYPE(TestOption)
Q_DECLARE_METATYPE(TestChoice)
QDBusArgument& operator<<(QDBusArgument& a, const TestRule& v)
{
    a.beginStructure();
    a << v.type << v.pattern;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, TestRule& v)
{
    a.beginStructure();
    a >> v.type >> v.pattern;
    a.endStructure();
    return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const TestFilter& v)
{
    a.beginStructure();
    a << v.name << v.rules;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, TestFilter& v)
{
    a.beginStructure();
    a >> v.name >> v.rules;
    a.endStructure();
    return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const TestOption& v)
{
    a.beginStructure();
    a << v.id << v.label;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, TestOption& v)
{
    a.beginStructure();
    a >> v.id >> v.label;
    a.endStructure();
    return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const TestChoice& v)
{
    a.beginStructure();
    a << v.id << v.label << v.options << v.selected;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, TestChoice& v)
{
    a.beginStructure();
    a >> v.id >> v.label >> v.options >> v.selected;
    a.endStructure();
    return a;
}
class FrontendResponse : public QObject {
    Q_OBJECT
public:
    bool delivered = false;
    uint response = 99;
    QVariantMap results;
public slots:
    void receive(uint code, const QVariantMap& values)
    {
        response = code;
        results = values;
        delivered = true;
    }
};
class TestPortal : public QObject {
    Q_OBJECT
private:
    QProcess bus;
    QString address;
    QDBusConnection server { QString() }, client { QString() }, stranger { QString() };
    QDBusPendingCall call(const QString& method, const QVariantMap& options = { },
        const QString& handle = "/org/freedesktop/portal/desktop/request/test/one",
        const QString& parent = { })
    {
        auto message = QDBusMessage::createMethodCall(
            "org.freedesktop.impl.portal.desktop.omafiles", "/org/freedesktop/portal/desktop",
            "org.freedesktop.impl.portal.FileChooser", method);
        message.setArguments({ QVariant::fromValue(QDBusObjectPath(handle)),
            QString("org.test.App"), parent, QString("Choose a file"), options });
        return client.asyncCall(message, 30000);
    }
    QDBusMessage completed(QDBusPendingCall pending)
    {
        QDBusPendingCallWatcher watcher(pending);
        QSignalSpy ready(&watcher, &QDBusPendingCallWatcher::finished);
        if (!watcher.isFinished() && !ready.wait(30000)) {
            QTest::qFail("The portal call did not complete.", __FILE__, __LINE__);
            return { };
        }
        const auto reply = watcher.reply();
        if (reply.errorName() == "org.freedesktop.DBus.Error.NoReply")
            QTest::qFail("The portal call timed out instead of replying.", __FILE__, __LINE__);
        return reply;
    }
private slots:
    void initTestCase()
    {
        QTest::defaultTryTimeout.store(std::chrono::milliseconds(30000), std::memory_order_relaxed);
        QVERIFY(qEnvironmentVariable("HOME").startsWith("/tmp/"));
        qDBusRegisterMetaType<TestRule>();
        qDBusRegisterMetaType<QList<TestRule>>();
        qDBusRegisterMetaType<TestFilter>();
        qDBusRegisterMetaType<QList<TestFilter>>();
        qDBusRegisterMetaType<TestOption>();
        qDBusRegisterMetaType<QList<TestOption>>();
        qDBusRegisterMetaType<TestChoice>();
        qDBusRegisterMetaType<QList<TestChoice>>();
        bus.start("dbus-daemon", { "--session", "--nofork", "--print-address=1" });
        QVERIFY(bus.waitForStarted());
        QTRY_VERIFY(bus.canReadLine());
        address = QString::fromUtf8(bus.readLine()).trimmed();
        server = QDBusConnection::connectToBus(address, "portal-server");
        client = QDBusConnection::connectToBus(address, "portal-client");
        stranger = QDBusConnection::connectToBus(address, "portal-stranger");
        QVERIFY(server.isConnected());
        QVERIFY(client.isConnected());
    }
    void cleanupTestCase()
    {
        QDBusConnection::disconnectFromBus("portal-stranger");
        QDBusConnection::disconnectFromBus("portal-client");
        QDBusConnection::disconnectFromBus("portal-server");
        bus.terminate();
        bus.waitForFinished();
    }
    void opensAndCancelsWithoutWriting()
    {
        Portal portal;
        QVERIFY(portal.start(server));
        auto pending = call("OpenFile");
        QTRY_VERIFY(portal.current());
        QCOMPARE(portal.current()->kind, "open");
        QCOMPARE(portal.current()->title, "Choose a file");
        portal.current()->cancel();
        const auto reply = completed(pending);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(reply.arguments()[0].toUInt(), 1u);
        QVERIFY(!portal.current());
    }
    void returnsEncodedUrisAfterValidation()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto path = fixture.path() + "/a #%.txt";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        Portal portal;
        QVERIFY(portal.start(server));
        auto pending = call("OpenFile");
        QTRY_VERIFY(portal.current());
        portal.current()->finish({ path });
        QTRY_VERIFY(!portal.current());
        const auto reply = completed(pending);
        QCOMPARE(reply.arguments()[0].toUInt(), 0u);
        const auto results = qdbus_cast<QVariantMap>(reply.arguments()[1]);
        QCOMPARE(results["uris"].toStringList(),
            QStringList { QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) });
        QCOMPARE(results["writable"].toBool(), false);
    }
    void refusesMalformedOptionsAndParents()
    {
        Portal portal;
        QVERIFY(portal.start(server));
        for (const auto& options : { QVariantMap { { "multiple", "true" } },
                 QVariantMap { { "current_folder", QByteArray("/tmp") } },
                 QVariantMap { { "current_name", "../escape" } },
                 QVariantMap { { "filters", QStringList { "*.png" } } } }) {
            QCOMPARE(completed(call("OpenFile", options)).type(), QDBusMessage::ErrorMessage);
            QVERIFY(!portal.current());
        }
        QCOMPARE(completed(call("OpenFile", { }, "/org/freedesktop/portal/desktop/request/test/one",
                               "x11:invalid"))
                     .type(),
            QDBusMessage::ErrorMessage);
    }
    void queuesRequestsAndProtectsCloseOwnership()
    {
        Portal portal;
        QVERIFY(portal.start(server));
        auto first = call("OpenFile");
        QTRY_VERIFY(portal.current());
        auto* one = portal.current();
        auto second = call("SaveFile", { }, "/org/freedesktop/portal/desktop/request/test/two");
        QCOMPARE(completed(call("OpenFile")).type(), QDBusMessage::ErrorMessage);
        auto close = QDBusMessage::createMethodCall("org.freedesktop.impl.portal.desktop.omafiles",
            "/org/freedesktop/portal/desktop/request/test/one",
            "org.freedesktop.impl.portal.Request", "Close");
        QCOMPARE(completed(stranger.asyncCall(close)).errorName(),
            "org.freedesktop.DBus.Error.AccessDenied");
        QCOMPARE(portal.current(), one);
        QCOMPARE(completed(client.asyncCall(close)).type(), QDBusMessage::ReplyMessage);
        QCOMPARE(completed(first).arguments()[0].toUInt(), 2u);
        QTRY_VERIFY(portal.current());
        QCOMPARE(portal.current()->kind, "save");
        portal.current()->cancel();
        QCOMPARE(completed(second).arguments()[0].toUInt(), 1u);
    }
    void requiresOverwriteConsentAndAllowsRetry()
    {
        QTemporaryDir fixture;
        const auto path = fixture.path() + "/held";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("held");
        file.close();
        Portal portal;
        QVERIFY(portal.start(server));
        auto pending = call("SaveFile");
        QTRY_VERIFY(portal.current());
        auto* r = portal.current();
        r->finish({ path });
        QTRY_VERIFY(!r->busy);
        QCOMPARE(r->overwritePaths, QStringList { path });
        r->confirmOverwrite(false);
        QVERIFY(r->overwritePaths.isEmpty());
        QVERIFY(portal.current());
        r->finish({ path });
        QTRY_VERIFY(!r->busy);
        r->confirmOverwrite(true);
        QCOMPARE(completed(pending).arguments()[0].toUInt(), 0u);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("held"));
    }
    void preservesTypedFiltersAndChoices()
    {
        QTemporaryDir fixture;
        const auto path = fixture.path() + "/note.txt";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const TestFilter all { "All", { { 0, "*" } } }, text { "Text", { { 0, "*.txt" } } };
        const QList<TestChoice> choices {
            { "encoding", "Encoding", { { "utf8", "UTF-8" }, { "ascii", "ASCII" } }, "utf8" },
            { "extra", "Extra", { }, "false" }
        };
        QVariantMap options { { "filters", QVariant::fromValue(QList<TestFilter> { all, text }) },
            { "current_filter", QVariant::fromValue(text) },
            { "choices", QVariant::fromValue(choices) } };
        Portal portal;
        QVERIFY(portal.start(server));
        auto pending = call("OpenFile", options);
        QTRY_VERIFY(portal.current());
        auto* r = portal.current();
        QCOMPARE(r->selectedFilter(), 1);
        QVERIFY(r->matches("note.txt"));
        QTRY_VERIFY(!r->matches("note.TXT"));
        QVERIFY(!r->matches("image.png"));
        r->setChoice("encoding", "ascii");
        r->setChoice("extra", "true");
        r->setChoice("extra", "invalid");
        r->finish({ path });
        QTRY_VERIFY(!portal.current());
        const auto reply = completed(pending);
        const auto results = qdbus_cast<QVariantMap>(reply.arguments()[1]);
        const auto selected = qdbus_cast<TestFilter>(results["current_filter"]);
        QCOMPARE(selected.name, "Text");
        QCOMPARE(selected.rules.first().pattern, "*.txt");
        const auto returned = qdbus_cast<QList<TestOption>>(results["choices"]);
        QCOMPARE(returned.size(), 2);
        QCOMPARE(returned[0].label, "ascii");
        QCOMPARE(returned[1].label, "true");
        auto standalone = call("OpenFile", { { "current_filter", QVariant::fromValue(text) } });
        QTRY_VERIFY(portal.current());
        QCOMPARE(portal.current()->filters.size(), 1);
        QCOMPARE(portal.current()->selectedFilter(), 0);
        portal.current()->cancel();
        completed(standalone);
        options["current_filter"] = QVariant::fromValue(TestFilter { "Text", { { 0, "*.png" } } });
        QCOMPARE(completed(call("OpenFile", options)).type(), QDBusMessage::ErrorMessage);
    }
    void savesOrderedUniqueBasenamesWithoutCreatingFiles()
    {
        QTemporaryDir fixture;
        QFile held(fixture.path() + "/note.txt");
        QVERIFY(held.open(QIODevice::WriteOnly));
        held.close();
        Portal portal;
        QVERIFY(portal.start(server));
        QVariantMap options { { "files",
            QVariant::fromValue(QList<QByteArray> { QByteArray("note.txt\0", 9),
                QByteArray("note.txt\0", 9), QByteArray(".hidden\0", 8), QByteArray("new.txt\0", 8),
                QByteArray("NEW.TXT\0", 8) }) } };
        auto pending = call("SaveFiles", options);
        QTRY_VERIFY(portal.current());
        auto* r = portal.current();
        QCOMPARE(r->files.size(), 5);
        r->finish({ fixture.path() });
        QTRY_VERIFY(!portal.current());
        const auto results = qdbus_cast<QVariantMap>(completed(pending).arguments()[1]);
        const auto uris = results["uris"].toStringList();
        QCOMPARE(uris.size(), 5);
        QCOMPARE(QUrl(uris[0]).fileName(), "note (1).txt");
        QCOMPARE(QUrl(uris[1]).fileName(), "note (2).txt");
        QCOMPARE(QUrl(uris[2]).fileName(), ".hidden");
        QCOMPARE(QUrl(uris[3]).fileName(), "new.txt");
        QCOMPARE(QUrl(uris[4]).fileName(), "NEW (1).TXT");
        QVERIFY(!QFile::exists(fixture.path() + "/note (1).txt"));
        options["files"] = QVariant::fromValue(QList<QByteArray> { QByteArray("../bad\0", 7) });
        QCOMPARE(completed(call("SaveFiles", options)).type(), QDBusMessage::ErrorMessage);
    }
    void cancelsDisconnectedSenders()
    {
        Portal portal;
        QVERIFY(portal.start(server));
        auto transient = QDBusConnection::connectToBus(address, "portal-transient");
        auto message = QDBusMessage::createMethodCall(
            "org.freedesktop.impl.portal.desktop.omafiles", "/org/freedesktop/portal/desktop",
            "org.freedesktop.impl.portal.FileChooser", "OpenFile");
        message.setArguments({ QVariant::fromValue(QDBusObjectPath(
                                   "/org/freedesktop/portal/desktop/request/test/disconnect")),
            QString("org.test.App"), QString(), QString("Open"), QVariantMap { } });
        auto pending = transient.asyncCall(message);
        QTRY_VERIFY(portal.current());
        QDBusConnection::disconnectFromBus("portal-transient");
        transient = QDBusConnection(QString());
        QTRY_VERIFY(!portal.current());
    }
    void advertisesVersionForProxyDiscovery()
    {
        Portal portal;
        QVERIFY(portal.start(server));
        auto message
            = QDBusMessage::createMethodCall("org.freedesktop.impl.portal.desktop.omafiles",
                "/org/freedesktop/portal/desktop", "org.freedesktop.DBus.Properties", "GetAll");
        message.setArguments({ QString("org.freedesktop.impl.portal.FileChooser") });
        auto reply = completed(client.asyncCall(message));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QCOMPARE(qdbus_cast<QVariantMap>(reply.arguments()[0])["version"].toUInt(), 4u);
    }
    void supportsDirectoryMultipleMimeAndSuggestedPaths()
    {
        QTemporaryDir fixture;
        const auto folder = fixture.path();
        const auto filePath = folder + "/one.txt";
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        Portal portal;
        QVERIFY(portal.start(server));
        QByteArray bytes = filePath.toUtf8();
        bytes.append('\0');
        const TestFilter text { "Text", { { 1, "text/plain" } } };
        auto pending = call("SaveFile",
            { { "current_file", bytes }, { "current_filter", QVariant::fromValue(text) } });
        QTRY_VERIFY(portal.current());
        auto* request = portal.current();
        QCOMPARE(request->currentFolder, folder);
        QCOMPARE(request->currentName, "one.txt");
        QTRY_VERIFY(!request->matches("image.png"));
        QVERIFY(request->matches("one.TXT"));
        request->cancel();
        completed(pending);
        auto directory = call("OpenFile", { { "directory", true } });
        QTRY_VERIFY(portal.current());
        portal.current()->finish({ folder });
        QTRY_VERIFY(!portal.current());
        QCOMPARE(completed(directory).arguments()[0].toUInt(), 0u);
        auto multiple = call("OpenFile", { { "multiple", true } });
        QTRY_VERIFY(portal.current());
        portal.current()->finish({ filePath, filePath });
        QTRY_VERIFY(!portal.current());
        QCOMPARE(qdbus_cast<QVariantMap>(completed(multiple).arguments()[1])["uris"]
                     .toStringList()
                     .size(),
            2);
    }
    void boundsTheQueueAndCancelsPendingValidation()
    {
        Portal portal;
        QVERIFY(portal.start(server));
        QList<QDBusPendingCall> pending;
        for (int i = 0; i < 64; ++i)
            pending << call("OpenFile", { },
                "/org/freedesktop/portal/desktop/request/test/queued" + QString::number(i));
        auto overflow
            = call("OpenFile", { }, "/org/freedesktop/portal/desktop/request/test/overflow");
        QCOMPARE(completed(overflow).type(), QDBusMessage::ErrorMessage);
        QVERIFY(portal.current());
        QTemporaryDir fixture;
        portal.current()->finish({ fixture.path() + "/missing" });
        QVERIFY(portal.current()->busy);
        for (int i = 0; i < 64; ++i) {
            QVERIFY(portal.current());
            portal.current()->cancel();
            QCOMPARE(completed(pending[i]).arguments()[0].toUInt(), 1u);
        }
        QVERIFY(!portal.current());
    }
    void acceptsGioBytePaths_data()
    {
        QTest::addColumn<int>("length");
        for (const auto length : { 23, 56, 57, 58, 70 })
            QTest::newRow(qPrintable(QString::number(length))) << length;
    }
    void acceptsGioBytePaths()
    {
        QFETCH(int, length);
        const QString folder = "/tmp/" + QString(length - 5, QChar('a'));
        Portal portal;
        QVERIFY(portal.start(server));
        QProcess caller;
        // Construct byte arrays explicitly: gdbus's text parser corrupts some ay lengths.
        const QString script = R"PY(
import sys
from gi.repository import Gio, GLib
connection = Gio.DBusConnection.new_for_address_sync(
    sys.argv[1], Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
    Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
options = {
    'current_folder': GLib.Variant('ay', list(sys.argv[2].encode()) + [0]),
    'filters': GLib.Variant('a(sa(us))', [('Text', [(0, '*.txt')]), ('All', [(0, '*')])]),
    'choices': GLib.Variant('a(ssa(ss)s)', [
        ('read', 'Read only', [], 'false'),
        ('encoding', 'Encoding', [('utf8', 'UTF-8'), ('latin1', 'Latin-1')], 'utf8')])}
reply = connection.call_sync('org.freedesktop.impl.portal.desktop.omafiles',
    '/org/freedesktop/portal/desktop', 'org.freedesktop.impl.portal.FileChooser',
    'OpenFile', GLib.Variant('(osssa{sv})', (
        '/org/freedesktop/portal/desktop/request/test/gio', 'org.test.App', '', 'Open', options)),
    GLib.VariantType.new('(ua{sv})'), Gio.DBusCallFlags.NONE, 30000, None)
assert reply.unpack()[0] == 1
)PY";
        caller.start("/usr/bin/python3", { "-c", script, address, folder });
        QVERIFY(caller.waitForStarted());
        QTRY_VERIFY(portal.current() || caller.state() == QProcess::NotRunning);
        const auto diagnostic = caller.readAllStandardError();
        QVERIFY2(portal.current(), diagnostic.constData());
        QCOMPARE(portal.current()->currentFolder, folder);
        QCOMPARE(portal.current()->filters.size(), 2);
        QCOMPARE(portal.current()->choiceList().size(), 2);
        portal.current()->cancel();
        QTRY_COMPARE(caller.state(), QProcess::NotRunning);
        QVERIFY2(caller.exitCode() == 0, caller.readAllStandardError().constData());
    }
    void forwardsThroughTheDesktopPortalFrontend()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto descriptor = fixture.path() + "/omafiles.portal";
        QFile registration(descriptor);
        QVERIFY(registration.open(QIODevice::WriteOnly));
        registration.write("[portal]\nDBusName=org.freedesktop.impl.portal.desktop.omafiles\n"
                           "Interfaces=org.freedesktop.impl.portal.FileChooser;\nUseIn=omafiles;\n");
        registration.close();
        QFile configuration(fixture.path() + "/portals.conf");
        QVERIFY(configuration.open(QIODevice::WriteOnly));
        configuration.write("[preferred]\ndefault=omafiles\norg.freedesktop.impl.portal.FileChooser=omafiles\n");
        configuration.close();
        const auto path = fixture.path() + "/selected.txt";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        Portal portal;
        QVERIFY(portal.start(server));
        QProcess frontend;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("DBUS_SESSION_BUS_ADDRESS", address);
        environment.insert("XDG_DESKTOP_PORTAL_DIR", fixture.path());
        environment.insert("XDG_CURRENT_DESKTOP", "omafiles");
        environment.insert("GSETTINGS_SCHEMA_DIR", "/usr/share/glib-2.0/schemas");
        environment.insert("GSETTINGS_BACKEND", "memory");
        frontend.setProcessEnvironment(environment);
        frontend.start("/usr/lib/xdg-desktop-portal", { });
        QVERIFY(frontend.waitForStarted());
        QTRY_VERIFY(client.interface()->isServiceRegistered("org.freedesktop.portal.Desktop")
            || frontend.state() == QProcess::NotRunning);
        QVERIFY2(frontend.state() != QProcess::NotRunning, frontend.readAllStandardError().constData());
        FrontendResponse response;
        QVERIFY(client.connect("org.freedesktop.portal.Desktop", QString(),
            "org.freedesktop.portal.Request", "Response", &response,
            SLOT(receive(uint,QVariantMap))));
        auto message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser", "OpenFile");
        message.setArguments({ QString(), QString("Choose through the frontend"),
            QVariantMap { { "handle_token", "integration" },
                { "current_folder", fixture.path().toUtf8() + '\0' } } });
        auto pending = client.asyncCall(message, 30000);
        QTRY_VERIFY(portal.current() || frontend.state() == QProcess::NotRunning);
        QVERIFY2(portal.current(), frontend.readAllStandardError().constData());
        QCOMPARE(portal.current()->title, "Choose through the frontend");
        QCOMPARE(portal.current()->currentFolder, fixture.path());
        const auto reply = completed(pending);
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        QVERIFY(reply.arguments()[0].value<QDBusObjectPath>().path().startsWith(
            "/org/freedesktop/portal/desktop/request/"));
        portal.current()->finish({ path });
        QTRY_VERIFY(response.delivered);
        QCOMPARE(response.response, 0u);
        QCOMPARE(qdbus_cast<QStringList>(response.results["uris"]),
            QStringList { QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded) });
        frontend.terminate();
        QVERIFY(frontend.waitForFinished());
        QTRY_VERIFY(!client.interface()->isServiceRegistered("org.freedesktop.portal.Desktop"));
    }
    void keepsInvalidSelectionsOpen()
    {
        QTemporaryDir fixture;
        Portal portal;
        QVERIFY(portal.start(server));
        auto pending = call("OpenFile");
        QTRY_VERIFY(portal.current());
        auto* r = portal.current();
        r->finish({ fixture.path() + "/missing" });
        QTRY_VERIFY(!r->busy);
        QVERIFY(!r->error.isEmpty());
        QVERIFY(portal.current());
        r->finish({ fixture.path() });
        QTRY_VERIFY(!r->busy);
        QCOMPARE(r->error, "Choose a file.");
        r->cancel();
        completed(pending);
    }
};
QTEST_GUILESS_MAIN(TestPortal)
#include "tst_portal.moc"
