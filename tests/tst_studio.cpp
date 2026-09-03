// The studio, driven by the keyboard, with no screen involved.
//
// This file exists because of one bug. Walking into a folder killed every key
// in the window: the state object emitted a single `changed()` for everything,
// that signal was the NOTIFY of the tab and pane list properties, and writing
// the new working directory therefore told QML the tab list had changed --
// so the Repeaters tore down the pane, the view holding the focus went with it,
// and the window stopped answering the keyboard while still looking correct.
//
// Nothing a unit test on the model could have caught, and nothing a screenshot
// shows. So the test drives the real window with real key events and asks the
// real state what happened.

#include "FolderModel.h"
#include "GraphItem.h"
#include "IndexModel.h"
#include "Open.h"
#include "Paths.h"
#include "Theme.h"
#include "Workspace.h"

#include <QAccessible>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QLockFile>
#include <QPainter>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtQml>
#include <QtTest>

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace omafiles;

// Nothing in this binary starts a program.
//
// It used to. A `chromium` sat for ten hours on `/tmp/tst_studio-OpeLNM/two.md`
// -- a fixture the run that opened it had already deleted -- and the journal has
// the same pair of launches on seven other runs. `drawsItself` is the route: it
// parks the cursor on `two.md`, presses `t` for the tag panel and then Enter
// twice, and asserted nothing about the handoff, so on the runs where the panel
// had not taken the keyboard both Enters activated the file row instead. The
// test passed either way.
//
// The rest of the file avoided the hazard by hand -- the graph test picks tag
// nodes, the key sweep leans on row 1 of `sweep` being a folder -- and avoiding
// a hazard only ever works for the tests that remember it is there. So the
// launcher is a seam now and this replaces it before `main` runs: a case added
// tomorrow cannot start a program either, whatever it presses, without anyone
// having to remember why.
//
// The requests are kept rather than dropped, because a recording is better
// coverage than an avoidance: `recordsAnOpenInsteadOfLaunchingIt` reads them.
static QVector<OpenRequest> &openRequests()
{
    static QVector<OpenRequest> recorded;
    return recorded;
}

static const bool recorderInstalled = [] {
    setLauncher([](const OpenRequest &request, QString *) {
        openRequests().append(request);
        return true;
    });
    return true;
}();

static bool indexModelHasTag(IndexModel *index, const QString &tag)
{
    for (int i = 0; i < index->tagRowCount(); ++i) {
        if (index->tagAt(i) == tag)
            return true;
    }
    return false;
}

// A hyprland config, written where the theme looks for one.
static bool writeLooknfeel(const QString &path, const QString &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    return file.write(body.toUtf8()) == body.toUtf8().size();
}

// Points the theme at a scratch desktop for the length of one test, and puts
// the real one back afterwards.
struct DesktopEnv
{
    explicit DesktopEnv(const QString &root)
        : config(qgetenv("XDG_CONFIG_HOME")), omarchy(qgetenv("OMARCHY_PATH"))
    {
        qputenv("XDG_CONFIG_HOME", QDir(root).filePath("config").toUtf8());
        qputenv("OMARCHY_PATH", QDir(root).filePath("omarchy").toUtf8());
    }
    ~DesktopEnv()
    {
        qputenv("XDG_CONFIG_HOME", config);
        qputenv("OMARCHY_PATH", omarchy);
    }
    const QByteArray config;
    const QByteArray omarchy;
};

// The shape omarchy ships as the user's override: the whole thing commented
// out, `rounding` included.
static QString commentedLooknfeel()
{
    return QStringLiteral("-- Change the default Omarchy look'n'feel.\n"
                          "-- hl.config({\n"
                          "--   decoration = {\n"
                          "--     -- Use round window corners.\n"
                          "--     rounding = 8,\n"
                          "--   },\n"
                          "-- })\n");
}

static QString looknfeelRounding(int rounding)
{
    return QStringLiteral("hl.config({\n"
                          "  decoration = {\n"
                          "    rounding = %1,\n"
                          "\n"
                          "    shadow = {\n"
                          "      enabled = false,\n"
                          "    },\n"
                          "  },\n"
                          "})\n")
        .arg(rounding);
}

class TestStudio : public QObject
{
    Q_OBJECT

private slots:
    void pickSortRoleTogglesOnSameRole();
    void initTestCase();
    void init();
    void cleanupTestCase();

    void walksIntoAFolderAndBackOut();
    void recordsAnOpenInsteadOfLaunchingIt();
    void keepsTheKeyboardAfterEveryStateChange();
    void putsTheCursorBackOnTheFolderItLeft();
    void keepsTheCursorOnTheRowWhileFiltering();
    void readsOutTheRowUnderTheCursor();
    void namesTheRowUnderTheCursorForAssistiveTech();
    void sortsByTagWithTheUntaggedLast();
    void countsWhatIsInAFolder();
    void countsWhatIsTaggedInThisFolder();
    void listsEveryFileCarryingATagWhereverItIs();
    void saysWhetherAFolderIsShutGoneOrEmpty();
    void listsTheNamesTheSystemWillNotDescribe();
    void saysWhenNothingInTheIndexCanBeReached();
    void sortsByTypeOnTheSuffix();
    void cyclesTheSortFieldAndItsOwnDirection();
    void showsHiddenFilesOnlyWhenAsked();
    void movesBetweenTheListAndTheTagPanel();
    void reachesControlsWithTabAndComesBackWithEscape();
    void worksTabsAndPanesFromTheKeyboard();
    void typesAPathAndAFilter();
    void walksTheGraphWithTheKeyboard();
    void runsACommandFromThePalette();
    void commandPaletteSortUsesPickSortRole();
    void zoomsAndResetsTheGraph();
    void picksAGraphNodeWithTheMouse();
    void clickingARowFocusesItsPane();
    void drawsEveryView();
    void dressesTheGraphInTheDesktopTheme();
    void saysHowMuchOfTheIndexTheGraphDrew();
    void saysWhereTheCursorIsOnAGraphTab();
    void readsTheCornerRoundingFromTheHyprlandConfig();
    void ignoresARoundingInsideALuaComment();
    void roundsNothingWithoutAHyprlandConfig();
    void followsTheDesktopsRoundingIntoQml();
    void showsTheKeySheetAndMarksARow();
    void survivesClosingAPaneWithoutComplaint();
    void noticesTheIndexChangingUnderIt();
    void watchesTheIndexOnAFreshInstall();
    void everyDocumentedKeyIsAnswered();
    void answersTheNamedAndChordedKeys();
    void drawsItself();
    void followsEveryThemeChangeAndNotJustTheFirst();
    void keepsMarksInTheTabTheyWereMadeIn();
    void followsTheFolderOnDisk();
    void countsEveryNameAFolderHolds();
    void writesTheDocumentationShots();
    void tagStripSaysHowManyTagsDoNotFit();

private:
    QQuickWindow *window() const;
    QList<QQuickItem *> itemsNamed(const QString &name) const;
    TabState *tab() const;
    QObject *filesRoot() const;
    int cursor() const;
    QString cwd() const;
    int rowOf(const QString &name) const;
    void focusRow(const QString &name);
    void key(char ch, Qt::KeyboardModifiers mods = Qt::NoModifier);
    void key(Qt::Key code, Qt::KeyboardModifiers mods = Qt::NoModifier);
    void settle();
    void writeIndex();
    /// Which set a picture belongs to. Only `Documentation` may write a name
    /// that `docs/img` uses.
    enum class Shot { Diagnostic, Documentation };
    void shot(const QString &name, Shot kind = Shot::Diagnostic);
    void tagDirectory(const QString &path, const QString &tag, bool reload = true);

    QTemporaryDir m_home;
    QTemporaryDir m_state;
    QTemporaryDir m_tree;
    QTemporaryDir m_desktop;
    Theme *m_theme = nullptr;
    IndexModel *m_index = nullptr;
    Workspace *m_workspace = nullptr;
    QQmlApplicationEngine *m_engine = nullptr;
};

// ------------------------------------------------------------------ fixture

// A real index, so the tag panel has something in it. The panel is walked by
// the keyboard, and a test that only ever sees an empty list proves nothing
// about walking it.
void TestStudio::writeIndex()
{
    const QDir data(m_home.path());
    QVERIFY(data.mkpath(QStringLiteral("omafiles")));

    QFile index(data.filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));

    const QDir tree(m_tree.path());
    const struct { const char *file; const char *tags; } rows[] = {
        {"one.txt", "\"notes\",\"work\""},
        {"two.md", "\"notes\""},
        {"three.ods", "\"project\",\"spreadsheet\""},
        {"four.drawio", "\"diagram\",\"project\""},
        {"five.zip", "\"archive\",\"backup\""},
        {"six.md", "\"notes\",\"project\",\"work\""},
        {"seven.txt", "\"backup\""},
    };
    for (const auto &row : rows) {
        const QString path = tree.filePath(QString::fromLatin1(row.file));
        const QFileInfo info(path);
        // Deliberately the old shape, with `mtime`, `dev` and `ino` still on it.
        // Nothing reads those any more, and the studio has to go on reading an
        // index that was written while they did.
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[%2],\"mtime\":%3,"
                                    "\"dev\":0,\"ino\":0}\n")
                        .arg(path, QString::fromLatin1(row.tags))
                        .arg(info.lastModified().toSecsSinceEpoch())
                        .toUtf8());
    }
}

void TestStudio::initTestCase()
{
    QVERIFY(m_home.isValid());
    QVERIFY(m_state.isValid());
    QVERIFY(m_tree.isValid());

    // The index and the theme both read the user's XDG directories. Pointed at
    // temporaries so a test run never sees, and never writes, real tags.
    qputenv("XDG_DATA_HOME", m_home.path().toUtf8());
    qputenv("XDG_STATE_HOME", m_state.path().toUtf8());
    // `~` and the tab's starting directory both come from $HOME. Pointed at the
    // fixture so the home key is a folder this test built rather than whatever
    // is in the home directory of whoever is running it.
    qputenv("HOME", m_tree.path().toUtf8());
    // The corner rounding comes from the hyprland config, so that gets a
    // fixture too -- the desktop this window is drawn against is the one below
    // and never the one the machine is running. Written the way omarchy ships
    // it: an override that is entirely commented out over a default of 0.
    QVERIFY(m_desktop.isValid());
    qputenv("XDG_CONFIG_HOME", QDir(m_desktop.path()).filePath("config").toUtf8());
    qputenv("OMARCHY_PATH", QDir(m_desktop.path()).filePath("omarchy").toUtf8());
    QVERIFY(writeLooknfeel(m_desktop.filePath("config/hypr/looknfeel.lua"),
                           commentedLooknfeel()));
    QVERIFY(writeLooknfeel(m_desktop.filePath("omarchy/default/hypr/looknfeel.lua"),
                           looknfeelRounding(0)));

    const QDir tree(m_tree.path());
    QVERIFY(tree.mkpath(QStringLiteral("alpha/inner")));
    // Somewhere for everyDocumentedKeyIsAnswered to work: not the home folder,
    // so `~` has somewhere to go; with a folder in it, so `l` has something to
    // enter; and with enough rows that `j`, `k` and `G` all land differently.
    QVERIFY(tree.mkpath(QStringLiteral("sweep/nested")));
    // The dotfile is named so it matches a `.txt` filter as well: the filter
    // used to reveal every hidden name whenever what you typed began with a dot.
    for (const QString &name : {QStringLiteral("sweep/one.txt"),
                                 QStringLiteral("sweep/two.txt"),
                                 QStringLiteral("sweep/three.txt"),
                                 QStringLiteral("sweep/.hidden.txt")}) {
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QVERIFY(tree.mkpath(QStringLiteral("beta")));
    for (const QString &name : {QStringLiteral("one.txt"), QStringLiteral("two.md"),
                                 QStringLiteral("three.ods"), QStringLiteral("four.drawio"),
                                 QStringLiteral("five.zip"), QStringLiteral("six.md"),
                                 QStringLiteral("seven.txt"),
                                 QStringLiteral("plain.txt")}) {   // deliberately untagged
        QFile file(tree.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    writeIndex();

    // Nothing is registered or injected here.
    //
    // All five types are declared in their headers and registered by the build,
    // and the three singletons are made by the engine on first use -- which is
    // during `load`, after the environment above is in place, so they still see
    // the temporary XDG directories and never the real ones. This block used to
    // be an exact copy of the studio's `main`, kept in step by memory.
    m_engine = new QQmlApplicationEngine(this);
    m_engine->load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    QVERIFY2(!m_engine->rootObjects().isEmpty(), "Main.qml did not load");

    // The same instances the window is bound to, not new ones: a test that made
    // its own would be driving a second copy of the state and asserting about a
    // window that never saw it.
    m_theme = m_engine->singletonInstance<Theme *>(QStringLiteral("omafiles"),
                                                    QStringLiteral("Theme"));
    m_index = m_engine->singletonInstance<IndexModel *>(QStringLiteral("omafiles"),
                                                         QStringLiteral("IndexModel"));
    m_workspace = m_engine->singletonInstance<Workspace *>(QStringLiteral("omafiles"),
                                                            QStringLiteral("Workspace"));
    QVERIFY(m_theme && m_index && m_workspace);
    QVERIFY(window());
    QVERIFY(QTest::qWaitForWindowExposed(window()));
}

void TestStudio::cleanupTestCase()
{
    delete m_engine;
    m_engine = nullptr;
}

// Each test starts from the same place: one files tab, at the top of the tree,
// unfiltered, list view, keyboard on the list.
void TestStudio::init()
{
    // A test that leaves a graph tab open would hand the next one a window with
    // no file list in it, and the failure would land on the wrong test.
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    while (m_workspace->paneCount() > 1)
        m_workspace->closePane(m_workspace->paneCount() - 1);
    for (int i = pane->tabCount() - 1; i >= 0; --i) {
        if (pane->tabCount() > 1 && pane->tabList().at(i)->kind() != QLatin1String("files"))
            m_workspace->closeTab(0, i);
    }
    m_workspace->setFocusedPane(0);
    pane->setCurrentIndex(0);
    settle();

    QVERIFY(tab());
    QCOMPARE(tab()->kind(), QStringLiteral("files"));
    tab()->setSelectedTag(QString());
    tab()->setFilter(QString());
    tab()->setViewMode(QStringLiteral("list"));
    tab()->setShowHidden(false);
    tab()->setSortRole(QStringLiteral("name"));
    tab()->setSortAscending(true);
    tab()->setCwd(m_tree.path());
    settle();

    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "takeFocus");
    settle();
    QVERIFY(filesRoot());
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    QCOMPARE(cwd(), m_tree.path());
}

QQuickWindow *TestStudio::window() const
{
    if (!m_engine || m_engine->rootObjects().isEmpty())
        return nullptr;
    return qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
}

// Everything on screen under a given objectName.
//
// Not QObject::findChildren: a delegate made by a Repeater or a Loader has no
// QObject parent at all -- only a visual one -- so searching the window's
// object tree finds the handful of items written directly in Main.qml and none
// of the ones a model produced. Panes and key-sheet entries are both the second
// kind, so the search has to be down the visual tree.
QList<QQuickItem *> TestStudio::itemsNamed(const QString &name) const
{
    QList<QQuickItem *> found;
    if (!window() || !window()->contentItem())
        return found;
    QList<QQuickItem *> queue{window()->contentItem()};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (item->objectName() == name)
            found.append(item);
        queue.append(item->childItems());
    }
    return found;
}

TabState *TestStudio::tab() const
{
    return m_workspace->currentTab();
}

QObject *TestStudio::filesRoot() const
{
    return m_engine->rootObjects().first()->property("activeFilesRoot").value<QObject *>();
}

int TestStudio::cursor() const
{
    QObject *root = filesRoot();
    return root ? root->property("cursor").toInt() : -1;
}

QString TestStudio::cwd() const
{
    return tab() ? tab()->cwd() : QString();
}

// Tests name the row they mean rather than counting `j` presses to it. Counting
// keystrokes reads like a user but tests like a hazard: the row a given number
// of presses lands on depends on the sort, the filter and where the cursor
// happened to be when the previous test finished.
int TestStudio::rowOf(const QString &name) const
{
    QObject *folder = filesRoot() ? filesRoot()->property("folder").value<QObject *>() : nullptr;
    if (!folder)
        return -1;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString path;
        QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path), Q_ARG(int, i));
        if (QFileInfo(path).fileName() == name)
            return i;
    }
    return -1;
}

void TestStudio::focusRow(const QString &name)
{
    const int row = rowOf(name);
    QVERIFY2(row >= 0, qPrintable(QStringLiteral("no row named %1").arg(name)));
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(row)));
    settle();
}

void TestStudio::key(char ch, Qt::KeyboardModifiers mods)
{
    QTest::keyClick(window(), ch, mods);
    settle();
}

void TestStudio::key(Qt::Key code, Qt::KeyboardModifiers mods)
{
    QTest::keyClick(window(), code, mods);
    settle();
}

// The cursor restore runs through `Qt.callLater`, so a bare `processEvents`
// once is not enough to see the window's final answer.
void TestStudio::settle()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// Renders the window to a file when OMAFILES_SHOTS names a directory. Off by
// default: the suite is about behaviour, and nobody wants a test that writes
// pictures every time it runs. With it on, a keyboard-driven window can be
// looked at without a screen, a session or a compositor.
// Writes one picture, if asked.
//
// `drawsItself` and `writesTheDocumentationShots` both take pictures, and two of
// the names collided: running the whole suite with OMAFILES_SHOTS=docs/img wrote
// `drawsItself`'s idea of `keys` and `graph` over the two committed images. The
// diagnostic ones go in a subdirectory of their own, so only the documentation
// case can write a name the documentation uses.
void TestStudio::shot(const QString &name, Shot kind)
{
    QString dir = QString::fromLocal8Bit(qgetenv("OMAFILES_SHOTS"));
    if (dir.isEmpty())
        return;
    if (kind == Shot::Diagnostic)
        dir += QStringLiteral("/diagnostic");
    QDir().mkpath(dir);

    // Park the pointer on empty background first. Offscreen it starts at the
    // window's top-left corner, which is a toolbar button, and by the second
    // picture it has hovered there long enough for the tooltip to open over
    // the tab strip.
    QTest::mouseMove(window(), QPoint(int(window()->width() * 0.35),
                                      int(window()->height() * 0.85)));

    // Long enough for the colour transitions to land, and for a tooltip asked
    // for earlier to have gone. Without the wait the picture catches every
    // control mid-fade and reads as though the bindings were stale.
    QTest::qWait(320);
    const QImage image = window()->grabWindow();
    QVERIFY(!image.isNull());
    QVERIFY(image.save(dir + QLatin1Char('/') + name + QStringLiteral(".png")));
}

// Appends a row to the index the way the command line would, without telling
// the window. `reload` says whether to hand it the news; the keys that exist to
// fetch it need a row it has not been handed.
void TestStudio::tagDirectory(const QString &path, const QString &tag, bool reload)
{
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::Append | QIODevice::Text));
    index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"%2\"]}\n")
                    .arg(path, tag)
                    .toUtf8());
    index.close();
    if (reload)
        m_index->reload();
}

// -------------------------------------------------------------------- tests

// The reported bug, in the order it was reported: go in, try to come back.
void TestStudio::walksIntoAFolderAndBackOut()
{
    key('j');
    QVERIFY2(cursor() > 0, "j did not move the cursor off the parent row");

    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));

    key('h');
    QCOMPARE(cwd(), m_tree.path());

    // And again, because the first `h` used to work by accident in some builds
    // while the window was already dead to everything after it.
    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    key(Qt::Key_Backspace);
    QCOMPARE(cwd(), m_tree.path());
}

// The other half of `l`: on a folder it walks in, and on a file it asks the
// desktop to open the thing.
//
// That half was asserted by nothing at all, because every test that could have
// reached it stepped around it instead -- and the one that stopped stepping
// around it, `drawsItself`, opened a browser on a temporary file eight times
// over two days without ever failing. With the launcher replaced there is a
// request to read, so each route is covered rather than avoided, and a route
// that stops working now says so.
void TestStudio::recordsAnOpenInsteadOfLaunchingIt()
{
    QObject *root = m_engine->rootObjects().first();
    const QString file = m_tree.filePath(QStringLiteral("two.md"));
    focusRow(QStringLiteral("two.md"));

    // Enter, which is what `drawsItself` was pressing.
    openRequests().clear();
    key(Qt::Key_Return);
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().at(0).kind == OpenRequest::Open,
             "Enter on a file asked for something other than an open");
    QCOMPARE(openRequests().at(0).path, file);
    // And it stayed where it was: a file is not a folder to walk into.
    QCOMPARE(cwd(), m_tree.path());

    // `l` and Right are the same move by two other spellings, and the key sheet
    // promises all three.
    key('l');
    QCOMPARE(openRequests().size(), 2);
    QCOMPARE(openRequests().at(1).path, file);
    key(Qt::Key_Right);
    QCOMPARE(openRequests().size(), 3);
    QCOMPARE(openRequests().at(2).path, file);

    // The row menu's own Open, which is a different road to the same place: it
    // goes through the row when the cursor is on it and calls the model
    // directly when it is not.
    openRequests().clear();
    key('m');
    QVERIFY2(root->property("blocked").toBool(), "m did not open the row menu");
    key(Qt::Key_Return);                        // Open is the first entry
    QCOMPARE(openRequests().size(), 1);
    QCOMPARE(openRequests().at(0).path, file);
    QCOMPARE(root->property("blocked").toBool(), false);

    // A file node on the graph hands its path straight to the model, with no
    // folder listing in between. This is the case `walksTheGraphWithTheKeyboard`
    // picks tag nodes to stay away from.
    openRequests().clear();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    QString node;
    for (int i = 0; i < 64 && node.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (id.startsWith(QLatin1String("file:")))
            node = id;
    }
    QVERIFY2(!node.isEmpty(), "the graph has no file node to activate");
    key('l');
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().at(0).kind == OpenRequest::Open,
             "activating a file node on the graph asked for something else");
    QCOMPARE(openRequests().at(0).path, node.mid(5));
    m_workspace->closeCurrentTab();
    settle();

    // Share goes through the same door. There is no key for it and the menu
    // entry only appears where LocalSend is installed, so it is asked for
    // directly -- a seam that caught the opens and let the shares through would
    // be no seam at all.
    openRequests().clear();
    m_index->sharePath(file);
    QCOMPARE(openRequests().size(), 1);
    QVERIFY2(openRequests().at(0).kind == OpenRequest::Share,
             "sharePath did not ask for a share");
    QCOMPARE(openRequests().at(0).path, file);

    // And a folder is not an open at all, which is the only reason the key
    // sweep has never launched anything.
    openRequests().clear();
    focusRow(QStringLiteral("alpha"));
    key(Qt::Key_Return);
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    QVERIFY2(openRequests().isEmpty(), "walking into a folder asked the desktop to open it");
    key('h');
    QCOMPARE(cwd(), m_tree.path());
}

// The failure was never really about `h`. Writing any tab state at all took the
// keyboard down with it, so the check is that the window still answers after
// each kind of write.
void TestStudio::keepsTheKeyboardAfterEveryStateChange()
{
    // Down if there is room, up otherwise: the point is that the key is
    // answered at all, and the cursor is sometimes already on the last row.
    const auto stillMoves = [this](const char *what) {
        const int before = cursor();
        key('j');
        if (cursor() == before)
            key('k');
        QVERIFY2(cursor() != before, what);
    };

    focusRow(QStringLiteral("alpha"));
    key('l');                       // change directory
    stillMoves("dead after entering a folder");

    key('h');                       // change directory back
    stillMoves("dead after leaving a folder");

    key('2');                       // change view mode
    QCOMPARE(tab()->viewMode(), QStringLiteral("details"));
    stillMoves("dead after switching view");

    key('1');
    key('s');                       // change sort
    QCOMPARE(tab()->sortRole(), QStringLiteral("tags"));
    stillMoves("dead after re-sorting");

    const bool wasAscending = tab()->sortAscending();
    key('S');
    QCOMPARE(tab()->sortAscending(), !wasAscending);
    stillMoves("dead after flipping the sort");

    key(Qt::Key_Space);             // change marks
    stillMoves("dead after marking a row");

    key('A');
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()->property("markedCount").toInt(), 0);
    stillMoves("dead after clearing marks");
}

// Walking out of a folder should land where you were, not at the top.
void TestStudio::putsTheCursorBackOnTheFolderItLeft()
{
    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));

    key('h');
    QCOMPARE(cwd(), m_tree.path());

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QString under;
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, under),
                              Q_ARG(int, cursor()));
    QCOMPARE(under, m_tree.filePath(QStringLiteral("alpha")));

    // Enter on the `..` row is the same move as `h`, and has to land in the
    // same place: two ways up that disagree is one of them being wrong.
    focusRow(QStringLiteral("alpha"));
    key('l');
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    key(Qt::Key_Return);
    QCOMPARE(cwd(), m_tree.path());
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, under),
                              Q_ARG(int, cursor()));
    QCOMPARE(under, m_tree.filePath(QStringLiteral("alpha")));
}

// A filter rebuilds the list under the cursor. The row you were on should stay
// under it for as long as the row is still in the list.
void TestStudio::keepsTheCursorOnTheRowWhileFiltering()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    focusRow(QStringLiteral("one.txt"));

    tab()->setFilter(QStringLiteral("one"));
    settle();

    QString under;
    QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, under),
                              Q_ARG(int, cursor()));
    QCOMPARE(under, m_tree.filePath(QStringLiteral("one.txt")));

    tab()->setFilter(QString());
    settle();
}

// The details panel reads one map off the model. If the map is right the panel
// is right; what it looks like is a separate question from what it knows.
void TestStudio::readsOutTheRowUnderTheCursor()
{
    focusRow(QStringLiteral("two.md"));

    const QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("name")).toString(), QStringLiteral("two.md"));
    QCOMPARE(entry.value(QStringLiteral("path")).toString(),
             m_tree.filePath(QStringLiteral("two.md")));
    QCOMPARE(entry.value(QStringLiteral("directory")).toBool(), false);
    QCOMPARE(entry.value(QStringLiteral("mime")).toString(), QStringLiteral("text/markdown"));
    QVERIFY(!entry.value(QStringLiteral("sizeText")).toString().isEmpty());
    QVERIFY(!entry.value(QStringLiteral("mtimeText")).toString().isEmpty());
    QCOMPARE(entry.value(QStringLiteral("tags")).toStringList(),
             QStringList{QStringLiteral("notes")});

    // A folder row reports itself as one rather than as a file of size zero.
    focusRow(QStringLiteral("alpha"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("directory")).toBool(), true);

    // Marking the row reads out straight away. Nothing here moves the cursor,
    // so nothing else would re-read the row, and the panel went on saying the
    // row was unmarked until you walked off it and back.
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QMetaObject::invokeMethod(folder, "clearMarks");
    focusRow(QStringLiteral("two.md"));
    const auto markedNow = [this] {
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("marked")).toBool();
    };
    QCOMPARE(markedNow(), false);
    key(Qt::Key_Space);
    QCOMPARE(markedNow(), true);
    key(Qt::Key_Space);
    QCOMPARE(markedNow(), false);
}

static QQuickItem *viewItemAt(QObject *view, int index)
{
    if (!view)
        return nullptr;
    QQuickItem *item = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex",
                              Q_RETURN_ARG(QQuickItem *, item),
                              Q_ARG(int, index));
    return item;
}


void TestStudio::namesTheRowUnderTheCursorForAssistiveTech()
{
    focusRow(QStringLiteral("two.md"));

    QObject *listing = filesRoot()->property("view").value<QObject *>();
    QVERIFY(listing);
    const int rowIndex = listing->property("currentIndex").toInt();
    QQuickItem *rowItem = viewItemAt(listing, rowIndex);
    QVERIFY(rowItem);
    QAccessibleInterface *rowIface = QAccessible::queryAccessibleInterface(rowItem);
    if (!rowIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    const QString rowName = rowIface->text(QAccessible::Name);
    QVERIFY2(rowName.contains(QStringLiteral("two.md")),
             qPrintable(QStringLiteral("row accessible name was: %1").arg(rowName)));

    const int tagCount = m_index->tagRowCount();
    QVERIFY2(tagCount >= 1, "the fixture index has no tag vocabulary");

    key('t');
    QObject *dock = m_engine->rootObjects().first()->property("activeDock").value<QObject *>();
    QVERIFY(dock);
    QObject *tags = dock->property("tagsList").value<QObject *>();
    QVERIFY(tags);
    const int tagIndex = tags->property("currentIndex").toInt();
    QVERIFY(tagIndex >= 0);
    QQuickItem *tagItem = viewItemAt(tags, tagIndex);
    QVERIFY(tagItem);
    QAccessibleInterface *tagIface = QAccessible::queryAccessibleInterface(tagItem);
    if (!tagIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    const QString tagName = tagIface->text(QAccessible::Name);
    QCOMPARE(tagName, m_index->tagAt(tagIndex));

    key(Qt::Key_Escape);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = m_engine->rootObjects().first()->property("activeGraph").value<QObject *>();
    QAccessibleInterface *graphIface = QAccessible::queryAccessibleInterface(
        qobject_cast<QQuickItem *>(graph));
    if (!graphIface)
        QSKIP("QAccessible cannot see QML attached Accessible names in offscreen");
    const QString graphName = graphIface->text(QAccessible::Name);
    QCOMPARE(graphName, QStringLiteral("Graph"));
}

// Sorting by tag is the one sort this program has that a file manager does not,
// and the useful half of it is where the untagged land: at the end going up, so
// you can read the tagged ones, and at the top going down, so you can find what
// still needs tagging.
void TestStudio::sortsByTagWithTheUntaggedLast()
{
    // `s` walks the fields in the order the table lays out its columns, and
    // comes back to where it started.
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("tags"));
    QCOMPARE(tab()->sortAscending(), true);

    const auto fileRows = [this] {
        QObject *folder = filesRoot()->property("folder").value<QObject *>();
        QStringList out;
        for (int i = 0; i < folder->property("count").toInt(); ++i) {
            QString kind;
            QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind),
                                      Q_ARG(int, i));
            if (kind != QLatin1String("file"))
                continue;
            QString path;
            QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                      Q_ARG(int, i));
            out << QFileInfo(path).fileName();
        }
        return out;
    };

    QStringList rows = fileRows();
    QVERIFY(rows.size() > 2);
    QCOMPARE(rows.last(), QStringLiteral("plain.txt"));
    QCOMPARE(rows.first(), QStringLiteral("five.zip"));      // #archive sorts first

    key('S');
    QCOMPARE(tab()->sortAscending(), false);
    rows = fileRows();
    QCOMPARE(rows.first(), QStringLiteral("plain.txt"));

    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("type"));
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("size"));
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    key('s');
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
}

// The type column carries the suffix, not the mime description: `.drawio` is
// `text/plain` to the mime database, and a column that called every diagram in
// the index "Plain text document" would be losing the distinction the tags are
// drawing.
void TestStudio::sortsByTypeOnTheSuffix()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();

    focusRow(QStringLiteral("four.drawio"));
    QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("type")).toString(), QStringLiteral("drawio"));
    QCOMPARE(entry.value(QStringLiteral("mime")).toString(), QStringLiteral("text/plain"));

    focusRow(QStringLiteral("alpha"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("type")).toString(), QStringLiteral("folder"));

    // Sorted by type, the files group by suffix.
    tab()->setSortRole(QStringLiteral("type"));
    tab()->setSortAscending(true);
    settle();

    QStringList types;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind), Q_ARG(int, i));
        if (kind != QLatin1String("file"))
            continue;
        QVariantMap row;
        QMetaObject::invokeMethod(folder, "entryAt", Q_RETURN_ARG(QVariantMap, row),
                                  Q_ARG(int, i));
        types << row.value(QStringLiteral("type")).toString();
    }
    QVERIFY(types.size() > 3);
    QStringList sorted = types;
    std::sort(sorted.begin(), sorted.end());
    QCOMPARE(types, sorted);

    tab()->setSortRole(QStringLiteral("name"));
    settle();
}

// The size column is blank for a folder, and what belongs there is what is in
// it. Counted for the focused row only, so the answer has to come off the same
// map the panel reads.
void TestStudio::countsWhatIsInAFolder()
{
    focusRow(QStringLiteral("alpha"));                       // holds `inner`
    QVariantMap entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("directory")).toBool(), true);
    QCOMPARE(entry.value(QStringLiteral("items")).toInt(), 1);

    focusRow(QStringLiteral("beta"));                        // holds nothing
    entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("items")).toInt(), 0);

    // A file is not counted; the panel shows its size instead.
    focusRow(QStringLiteral("two.md"));
    entry = filesRoot()->property("entry").toMap();
    QCOMPARE(entry.value(QStringLiteral("directory")).toBool(), false);
    QCOMPARE(entry.value(QStringLiteral("items")).toInt(), -1);
}

// The number that ends the confusion between the two scopes on screen: the
// header counts the whole index, and standing in a folder with none of it, that
// reads as a promise about the list in front of you.
void TestStudio::countsWhatIsTaggedInThisFolder()
{
    QObject *folder = filesRoot()->property("folder").value<QObject *>();

    // The fixture tags every file here but `plain.txt`.
    const int here = folder->property("taggedCount").toInt();
    QVERIFY2(here > 0, "the fixture folder has tagged files and this says none");
    QVERIFY2(here < folder->property("count").toInt(),
             "every row counted as tagged, including the folders and plain.txt");

    // A folder with nothing tagged in it says so, while the index still holds
    // plenty -- which is the case that made this necessary.
    tab()->setCwd(m_tree.filePath(QStringLiteral("alpha")));
    settle();
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()
                 ->property("taggedCount").toInt(), 0);
    QVERIFY(m_index->tagRowCount() > 0);

    // A tagged folder counts, and shows its tags. `omafiles tag add -- some-dir`
    // has always worked; the window used to skip the lookup for directories and
    // show nothing.
    tab()->setCwd(m_tree.path());
    settle();
    const int before = filesRoot()->property("folder").value<QObject *>()
                           ->property("taggedCount").toInt();
    tagDirectory(m_tree.filePath(QStringLiteral("beta")), QStringLiteral("shelf"));
    settle();
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()
                 ->property("taggedCount").toInt(), before + 1);
    focusRow(QStringLiteral("beta"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("tags")).toStringList(),
             QStringList{QStringLiteral("shelf")});

    // And the number reaches the status bar, which is the whole reason it
    // exists. Asserting only on the model left the on-screen half unmeasured.
    QObject *status = m_engine->rootObjects().first()
                          ->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY2(status, "the window has no status bar to read");
    QCOMPARE(status->property("tagged").toInt(),
             filesRoot()->property("folder").value<QObject *>()
                 ->property("taggedCount").toInt());
    QVERIFY(status->property("tagged").toInt() > 0);
}

// A tag is not folder-scoped, and every comparable tool says so: Dolphin lists
// `tags:/work` out of Baloo, TMSU mounts `tags/<tag>/`, Tagsistant does the
// same, and Finder's own wording for a tag is "regardless of which folder they
// are in". This program's `omafiles ls --tag` walks the whole index too, so a
// window that could only answer for one directory disagreed with the command
// line beside it about who carries a tag.
void TestStudio::listsEveryFileCarryingATagWhereverItIs()
{
    // The fixture index goes back whatever happens below: this case writes rows
    // for files outside the tree, and one left behind would land on every test
    // after it.
    const auto restore = qScopeGuard([this] {
        writeIndex();
        m_index->reload();
    });

    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    const QString kept = QDir(elsewhere.path()).filePath(QStringLiteral("kept.md"));
    {
        QFile file(kept);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    // The directory carries the tag as well -- `omafiles tag add -- some-dir`
    // works -- so the listing has a folder row to walk into.
    tagDirectory(kept, QStringLiteral("work"), false);
    tagDirectory(elsewhere.path(), QStringLiteral("work"));
    settle();

    // Standing in a folder with nothing tagged in it, so a listing that reads
    // the directory cannot pass this.
    const QString beta = m_tree.filePath(QStringLiteral("beta"));
    tab()->setCwd(beta);
    settle();
    tab()->setSelectedTag(QStringLiteral("work"));
    settle();

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(folder);

    // The rows, against the rule `omafiles ls --tag` follows: every index row
    // carrying the tag whose file the system will describe, and nothing else.
    QStringList listed;
    for (int i = 0; i < folder->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(folder, "kindAt", Q_RETURN_ARG(QString, kind),
                                  Q_ARG(int, i));
        QVERIFY2(kind != QLatin1String("parent"), "a tag listing offered a `..` row");
        QString path;
        QMetaObject::invokeMethod(folder, "pathAt", Q_RETURN_ARG(QString, path),
                                  Q_ARG(int, i));
        listed.append(path);
    }
    QStringList expected;
    for (const Record &record : m_index->records()) {
        if (record.tags.contains(QLatin1String("work"))
            && pathState(record.path, nullptr) == PathState::Present)
            expected.append(record.path);
    }
    listed.sort();
    expected.sort();
    QCOMPARE(listed, expected);
    QVERIFY2(listed.contains(kept),
             "the listing stopped at the folder the tag was picked in");
    QVERIFY2(listed.size() > 2, "this tag has too few files to prove anything");

    // And it is named as a tag everywhere it is named, rather than dressed as a
    // folder the window is not standing in.
    QCOMPARE(tab()->title(), QStringLiteral("#work"));
    const QList<QQuickItem *> crumbs = itemsNamed(QStringLiteral("pathCrumbs"));
    QCOMPARE(crumbs.size(), 1);
    QCOMPARE(crumbs.first()->property("full").toString(), QStringLiteral("#work"));

    // Not a place in the tree: there is nothing above a tag, so the key that
    // walks out of a folder has nothing to do and the tab stays where it is.
    QVERIFY(folder->property("atRoot").toBool());
    key('h');
    QCOMPARE(tab()->selectedTag(), QStringLiteral("work"));
    QCOMPARE(cwd(), beta);

    // The index is what reloads this list, not a directory watch: a row the
    // command line adds shows up with no key pressed.
    const QString later = QDir(elsewhere.path()).filePath(QStringLiteral("later.txt"));
    {
        QFile file(later);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    tagDirectory(later, QStringLiteral("work"));
    settle();
    QVERIFY2(rowOf(QStringLiteral("later.txt")) >= 0,
             "a row the index gained never reached the tag listing");

    // Walking into a folder asks for that folder, so it lets the tag go. The
    // tab's directory was never moved by the tag, so `beta` was still there to
    // come back to until this.
    focusRow(QFileInfo(elsewhere.path()).fileName());
    key('l');
    QCOMPARE(tab()->selectedTag(), QString());
    QCOMPARE(cwd(), elsewhere.path());

    tab()->setCwd(m_tree.path());
    settle();
}

// Three folders that draw the same list: one nobody may open, one that is not
// there, and one that is simply empty. The status line is what tells them apart,
// and each of the first two has to leave a `..` row to walk out by.
void TestStudio::saysWhetherAFolderIsShutGoneOrEmpty()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY2(status, "the window has no status bar to read");
    const auto folder = [this] {
        return filesRoot()->property("folder").value<QObject *>();
    };
    const auto complaint = [&] { return folder()->property("error").toString(); };

    // Empty and readable says nothing. That is the answer the other two were
    // being confused with, so it has to stay silent for the difference to mean
    // anything.
    tab()->setCwd(m_tree.filePath(QStringLiteral("beta")));
    settle();
    QCOMPARE(folder()->property("count").toInt(), 1);          // `..` and nothing else
    QCOMPARE(complaint(), QString());
    QCOMPARE(status->property("message").toString(), QString());

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString shut = QDir(scratch.path()).filePath(QStringLiteral("shut"));
    QVERIFY(QDir().mkpath(shut));
    QVERIFY(QFile::setPermissions(shut, QFileDevice::Permissions()));
    // Unconditional, so a failed assertion below still leaves a directory the
    // temporary can remove.
    const auto reopen = qScopeGuard([&shut] {
        QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
    });

    tab()->setCwd(shut);
    settle();
    QCOMPARE(cwd(), shut);
    QVERIFY2(complaint().contains(QStringLiteral("Permission denied")),
             "a folder nobody may open drew as an empty one");
    QCOMPARE(status->property("message").toString(), complaint());
    // And out again, which is `h` -- the row list is only ever `..` here.
    QCOMPARE(folder()->property("count").toInt(), 1);
    key('h');
    QCOMPARE(cwd(), scratch.path());

    // Not there at all. A typed path is the only way to reach one, and the tab
    // is where a typed path lands.
    const QString gone = QDir(scratch.path()).filePath(QStringLiteral("gone"));
    tab()->setCwd(gone);
    settle();
    QCOMPARE(cwd(), gone);
    QVERIFY2(complaint().contains(QStringLiteral("path missing")),
             "a folder that is not there drew as an empty one");
    QCOMPARE(status->property("message").toString(), complaint());
    // The way out is a row, not just a shortcut: Enter on `..` has to work from
    // a directory that does not exist, because that is where you are standing.
    QCOMPARE(folder()->property("count").toInt(), 1);
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    key(Qt::Key_Return);
    QCOMPARE(cwd(), scratch.path());

    tab()->setCwd(m_tree.path());
    settle();
}

// A dangling symlink, a symlink round to itself and a fifo are all names the
// folder holds, and Qt's own listing drops every one of them, so a folder of six
// things can list one and look complete. A name whose bytes are not valid UTF-8
// is the other half: that one cannot be spelled at all, so it is counted and
// named back mangled rather than listed as a row nothing could act on.
void TestStudio::listsTheNamesTheSystemWillNotDescribe()
{
    QObject *status = m_engine->rootObjects().first()
                          ->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QDir odd(scratch.path());
    {
        QFile file(odd.filePath(QStringLiteral("regular.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QVERIFY(QFile::link(QStringLiteral("/nowhere/at/all"),
                        odd.filePath(QStringLiteral("dangling"))));
    QVERIFY(QFile::link(QStringLiteral("loop"), odd.filePath(QStringLiteral("loop"))));
    QCOMPARE(::mkfifo(QFile::encodeName(odd.filePath(QStringLiteral("pipe"))).constData(),
                      0644), 0);
    // Written with open(2): nothing that takes a QString can spell this name.
    const QByteArray unnameable = QFile::encodeName(scratch.path()) + "/bad\xffname.txt";
    const int fd = ::open(unnameable.constData(), O_CREAT | O_WRONLY, 0644);
    QVERIFY(fd >= 0);
    ::close(fd);
    // QDir cannot see this name, so it cannot remove it either. Unlinked here
    // whatever happens below, or the temporary outlives the run.
    const auto sweep = qScopeGuard([&unnameable] { ::unlink(unnameable.constData()); });

    tab()->setCwd(scratch.path());
    settle();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();

    // `..` and four names, one of which is the only regular file here.
    QCOMPARE(folder->property("count").toInt(), 5);
    for (const QString &name : {QStringLiteral("regular.txt"), QStringLiteral("dangling"),
                                 QStringLiteral("loop"), QStringLiteral("pipe")}) {
        QVERIFY2(rowOf(name) >= 0,
                 qPrintable(QStringLiteral("%1 was not listed").arg(name)));
    }

    // What each row says it is. A link that leads nowhere and one that leads
    // round to itself are the same answer: it is a link and there is nothing at
    // the far end.
    const auto typeOfRow = [this](const QString &name) {
        focusRow(name);
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("type")).toString();
    };
    QCOMPARE(typeOfRow(QStringLiteral("regular.txt")), QStringLiteral("txt"));
    QCOMPARE(typeOfRow(QStringLiteral("dangling")), QStringLiteral("broken link"));
    QCOMPARE(typeOfRow(QStringLiteral("loop")), QStringLiteral("broken link"));
    QCOMPARE(typeOfRow(QStringLiteral("pipe")), QStringLiteral("special"));

    // The name that is bytes and not text, said rather than stepped over.
    QVERIFY2(folder->property("error").toString().contains(QStringLiteral("not valid UTF-8")),
             "a name the window cannot spell was dropped in silence");
    QCOMPARE(status->property("message").toString(), folder->property("error").toString());

    // Activating a fifo hands the desktop a read that may never return, so it
    // is refused here and said out loud.
    openRequests().clear();
    focusRow(QStringLiteral("pipe"));
    key(Qt::Key_Return);
    QVERIFY2(openRequests().isEmpty(), "a fifo was handed to the desktop's opener");
    QVERIFY2(folder->property("error").toString()
                 .contains(QStringLiteral("not a regular file")),
             "activating a fifo did nothing and said nothing");

    // The dangling link does go to the opener, because the opener is what names
    // both ends of it. The launcher is a seam in this binary, so the refusal is
    // asked of the plan the desktop would have been handed.
    OpenPlan plan;
    QString why;
    QVERIFY2(!planOpen(odd.filePath(QStringLiteral("dangling")), &plan, &why),
             "a link to nothing planned an open");
    QVERIFY(why.contains(QStringLiteral("/nowhere/at/all")));

    tab()->setCwd(m_tree.path());
    settle();
}

// An index with rows in it and nothing reachable is not an index with nothing in
// it, and `0 tags · 0 tagged` over an empty tag panel says it is. The counts mean
// what `omafiles ls` and `omafiles tags` mean -- rows the system will describe --
// so a count that came up short has to say so, and the graph has to name the
// repair that state wants rather than `tag add`.
void TestStudio::saysWhenNothingInTheIndexCanBeReached()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);
    const QString index = QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl"));

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString shut = QDir(scratch.path()).filePath(QStringLiteral("shut"));
    QVERIFY(QDir().mkpath(shut));
    QStringList held;
    for (const QString &name : {QStringLiteral("held.txt"), QStringLiteral("kept.md")}) {
        const QString path = QDir(shut).filePath(name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        held.append(path);
    }
    QVERIFY(QFile::setPermissions(shut, QFileDevice::Permissions()));
    // The chmod and the fixture index both go back whatever happens below. This
    // case writes an index of rows pointing nowhere, and a failed assertion that
    // left one behind would land on every test after it.
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
        writeIndex();
        m_index->reload();
    });
    const QStringList vanished = {QDir(scratch.path()).filePath(QStringLiteral("was-here.txt")),
                                  QDir(scratch.path()).filePath(QStringLiteral("also-here.md"))};

    const auto rewriteIndex = [&](const QStringList &paths) {
        QFile file(index);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        for (const QString &path : paths) {
            file.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"work\"]}\n")
                           .arg(path).toUtf8());
        }
        file.close();
        m_index->reload();
        settle();
    };

    // Rows the system declines to describe. `prune` must not be the advice for
    // these -- the files are there and behind a chmod.
    rewriteIndex(held);
    QCOMPARE(m_index->fileCount(), 0);
    QCOMPARE(m_index->unknownRows(), 2);
    QCOMPARE(m_index->tagRowCount(), 0);
    QVERIFY2(m_index->error().contains(QStringLiteral("not counted")),
             "the header counted nothing and the window said nothing");
    QCOMPARE(status->property("message").toString(), m_index->error());

    // Rows whose files are simply gone, which is the other answer and the one
    // `prune` settles.
    rewriteIndex(vanished);
    QCOMPARE(m_index->fileCount(), 0);
    QCOMPARE(m_index->missingRows(), 2);
    QVERIFY2(m_index->error().contains(QStringLiteral("omafiles prune")),
             "an index of rows pointing nowhere named no repair");

    // And the graph, which prints the advice. Three states, three pictures: an
    // index with nothing in it wants `tag add`, and the other two do not.
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    const auto graphShot = [this] {
        settle();
        QTest::qWait(60);
        const QImage shot = window()->grabWindow();
        return shot.copy(QRect(20, 110, qMax(10, shot.width() - 320),
                               qMax(10, shot.height() - 170)));
    };
    const QImage allGone = graphShot();
    rewriteIndex(held);
    const QImage allShut = graphShot();
    rewriteIndex(QStringList());
    const QImage nothingTagged = graphShot();

    QVERIFY2(allGone != nothingTagged,
             "an index of rows whose files are gone advised `omafiles tag add`");
    QVERIFY2(allShut != allGone,
             "a row the system will not describe got the advice for one that is gone");

    m_workspace->closeCurrentTab();
    settle();
}

// `s` picks the field and the direction that field is read in -- A to Z for a
// name, biggest first for a size, newest first for a date -- so it does not keep
// the direction `S` set for the field before it. That is a decision and not a
// slip: Dolphin and nnn both do it, and the key sheet says so. This is what
// holds the program to what the sheet says.
void TestStudio::cyclesTheSortFieldAndItsOwnDirection()
{
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
    QCOMPARE(tab()->sortAscending(), true);

    key('S');
    QCOMPARE(tab()->sortAscending(), false);

    key('s');                                   // name -> tags, a vocabulary reads A to Z
    QCOMPARE(tab()->sortRole(), QStringLiteral("tags"));
    QVERIFY2(tab()->sortAscending(), "s kept the direction set for the field before it");

    key('s');                                   // tags -> type
    key('s');                                   // type -> size, biggest first
    QCOMPARE(tab()->sortRole(), QStringLiteral("size"));
    QVERIFY2(!tab()->sortAscending(), "s did not start size at the biggest row");

    key('S');
    QCOMPARE(tab()->sortAscending(), true);
    key('s');                                   // size -> modified, newest first
    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    QVERIFY2(!tab()->sortAscending(), "s kept the direction set for the field before it");
}

// Dotfiles are hidden until a key asks for them, and a filter is only a filter:
// narrowing the list to `.txt` must not also change what the list is allowed to
// hold, or looking for text files turns up every hidden name in the folder and
// nothing says why.
void TestStudio::showsHiddenFilesOnlyWhenAsked()
{
    tab()->setCwd(m_tree.filePath(QStringLiteral("sweep")));
    settle();

    QCOMPARE(tab()->showHidden(), false);
    QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) < 0, "a dotfile was listed unasked");

    // The bare key nnn uses, and the chord GTK, Nautilus, Dolphin and ranger use.
    for (int i = 0; i < 2; ++i) {
        const char *which = i == 0 ? "`.`" : "Ctrl+H";
        const auto press = [this, i] {
            if (i == 0)
                key('.');
            else
                key(Qt::Key_H, Qt::ControlModifier);
        };

        press();
        QVERIFY2(tab()->showHidden(),
                 qPrintable(QStringLiteral("%1 did not ask for the hidden files")
                                .arg(QLatin1String(which))));
        QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) >= 0, "the dotfile is still not listed");

        press();
        QCOMPARE(tab()->showHidden(), false);
        QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) < 0, "the dotfile stayed listed");
    }

    // And a filter that begins with a dot is a filter and nothing else.
    tab()->setFilter(QStringLiteral(".txt"));
    settle();
    QVERIFY2(rowOf(QStringLiteral("one.txt")) >= 0, "the filter dropped a row it matches");
    QVERIFY2(rowOf(QStringLiteral(".hidden.txt")) < 0,
             "a filter beginning with a dot showed the hidden files");
    tab()->setFilter(QString());
    settle();
}

// `t` hands the keyboard to the tag panel and Escape takes it back. Both halves
// matter: a panel you can enter and not leave is worse than one you cannot
// reach at all.
void TestStudio::movesBetweenTheListAndTheTagPanel()
{
    QObject *root = m_engine->rootObjects().first();
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));
    const int tagCount = m_index->tagRowCount();
    QVERIFY2(tagCount >= 3, "the fixture index has no tag vocabulary to walk");

    key('t');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));

    QObject *dock = root->property("activeDock").value<QObject *>();
    QVERIFY2(dock, "the tag panel is not the one the keyboard is talking to");
    QObject *tags = dock->property("tagsList").value<QObject *>();
    QVERIFY(tags);
    QCOMPARE(tags->property("currentIndex").toInt(), 0);

    key('j');
    QCOMPARE(tags->property("currentIndex").toInt(), 1);
    key('k');
    QCOMPARE(tags->property("currentIndex").toInt(), 0);
    key('G');
    QCOMPARE(tags->property("currentIndex").toInt(), tagCount - 1);

    // Enter picks the tag the cursor is on, and picking it again lets it go.
    key(Qt::Key_Return);
    QCOMPARE(tab()->selectedTag(), m_index->tagAt(tagCount - 1));
    key(Qt::Key_Return);
    QCOMPARE(tab()->selectedTag(), QString());

    key(Qt::Key_Escape);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after a trip to the tag panel");

    // The way out of a tag listing is the way in: `t` puts the cursor on the tag
    // the listing is showing, so Enter lets it go. Landing on row 0 instead
    // leaves the keyboard hunting the vocabulary for the tag it is already
    // looking at.
    tab()->setSelectedTag(m_index->tagAt(1));
    settle();
    key('t');
    QCOMPARE(tags->property("currentIndex").toInt(), 1);
    key(Qt::Key_Return);
    QCOMPARE(tab()->selectedTag(), QString());
    key(Qt::Key_Escape);

    // The other two ways out. `h` and Left are what the rest of the window uses
    // for "back", the panel answers both, and nothing pressed either of them --
    // so they could stop working and the panel would still look reachable.
    for (int i = 0; i < 2; ++i) {
        key('t');
        QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));
        if (i == 0)
            key('h');
        else
            key(Qt::Key_Left);
        QVERIFY2(root->property("focusRegion").toString() == QLatin1String("list"),
                 i == 0 ? "h did not leave the tag panel"
                        : "Left did not leave the tag panel");
        // And it left the list where it was rather than walking up a folder.
        QCOMPARE(cwd(), m_tree.path());
    }

    // Half a page belongs to whichever list the keyboard is in. The chord block
    // runs before any region is consulted, so Ctrl+D and Ctrl+U decided there
    // walk the file list from inside the panel -- a cursor nobody can see move,
    // moving behind the reader's back.
    key('t');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));
    tags->setProperty("currentIndex", 0);
    const int fileCursor = cursor();
    key(Qt::Key_D, Qt::ControlModifier);
    QVERIFY2(tags->property("currentIndex").toInt() > 0, "Ctrl+D did not walk the tag list");
    QVERIFY2(cursor() == fileCursor, "Ctrl+D in the tag panel moved the file cursor");
    key(Qt::Key_U, Qt::ControlModifier);
    QCOMPARE(tags->property("currentIndex").toInt(), 0);
    QVERIFY2(cursor() == fileCursor, "Ctrl+U in the tag panel moved the file cursor");
    key(Qt::Key_Escape);

    // And the panel answers `t` in a pane too narrow to keep it drawn. The
    // window opens at 1180, one split leaves each pane under the 640 the dock
    // needs beside a list, and the sheet promises `t` with no size on it -- so
    // the size the program starts at is a size where a documented key has to
    // work.
    QMetaObject::invokeMethod(m_workspace, "splitRight", Q_ARG(QString, QStringLiteral("files")));
    settle();
    const QList<QQuickItem *> panes = itemsNamed(QStringLiteral("pane"));
    QCOMPARE(panes.size(), 2);
    QVERIFY2(panes.at(1)->width() < 640,
             "the split panes are wide enough for the dock, so this proves nothing");
    key('t');
    QVERIFY2(root->property("focusRegion").toString() == QLatin1String("tags"),
             "t did nothing in a pane too narrow to draw the tag panel");
    key(Qt::Key_Escape);
    QMetaObject::invokeMethod(m_workspace, "closePane", Q_ARG(int, 1));
    settle();
}

// Tab has to reach the controls, and Escape has to bring the keyboard back from
// wherever Tab left it. This is the whole of what "reachable by keyboard" means
// for the chips along the top.
void TestStudio::reachesControlsWithTabAndComesBackWithEscape()
{
    QObject *root = m_engine->rootObjects().first();
    QQuickItem *before = window()->activeFocusItem();

    QSet<QQuickItem *> visited;
    QQuickItem *reached = nullptr;
    for (int i = 0; i < 8; ++i) {
        key(Qt::Key_Tab);
        QQuickItem *now = window()->activeFocusItem();
        if (now && now != before) {
            visited.insert(now);
            reached = now;
        }
    }
    QVERIFY2(visited.size() >= 3, "Tab did not reach at least three controls");
    QVERIFY2(window()->activeFocusItem() == reached,
             "Tab did not leave the keyboard on a control");

    key(Qt::Key_Escape);

    // Measured on where the focus actually is. The two obvious assertions --
    // that `focusRegion` is "list" and that `j` still moves the cursor -- are
    // both true with Escape doing nothing at all: Tab never changed the region
    // in the first place, and a key pressed on a focused chip bubbles up to the
    // window's own handler anyway. So the half this test is named for used to
    // pass with the Escape branch disabled.
    QVERIFY2(window()->activeFocusItem() != reached,
             "Escape left the keyboard on the control Tab had reached");
    QCOMPARE(window()->activeFocusItem(), before);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    const int cursorBefore = cursor();
    key('j');
    QVERIFY2(cursor() != cursorBefore, "Escape did not give the keyboard back to the list");
}

// Tabs and panes are the part of the window with no other way in: there is a
// `+` and a context menu for making a tab, and nothing at all on screen for
// moving between panes. If these keys do not work, that half of the studio is
// mouse-only.
void TestStudio::worksTabsAndPanesFromTheKeyboard()
{
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    QCOMPARE(pane->tabCount(), 1);

    key('t', Qt::ControlModifier);
    QCOMPARE(pane->tabCount(), 2);
    QCOMPARE(pane->currentIndex(), 1);

    key('t', Qt::ControlModifier);
    QCOMPARE(pane->tabCount(), 3);

    key('[');
    QCOMPARE(pane->currentIndex(), 1);
    key(']');
    QCOMPARE(pane->currentIndex(), 2);
    key(']');                                   // wraps
    QCOMPARE(pane->currentIndex(), 0);

    key(Qt::Key_2, Qt::AltModifier);
    QCOMPARE(pane->currentIndex(), 1);

    key(Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(pane->currentIndex(), 2);
    key(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(pane->currentIndex(), 1);

    key('w', Qt::ControlModifier);
    QCOMPARE(pane->tabCount(), 2);

    // Delete on a focused tab handle closes it -- the same action as Ctrl w,
    // for when the keyboard is on the strip rather than the list.
    {
        const int tabsBefore = pane->tabCount();
        QQuickItem *handle = nullptr;
        for (int i = 0; i < 16; ++i) {
            key(Qt::Key_Tab);
            QQuickItem *now = window()->activeFocusItem();
            if (now && now->objectName() == QLatin1String("tabHandle")) {
                handle = now;
                break;
            }
        }
        QVERIFY2(handle, "Tab did not reach a tab handle");
        key(Qt::Key_Delete);
        QCOMPARE(pane->tabCount(), tabsBefore - 1);
    }
    QMetaObject::invokeMethod(m_engine->rootObjects().first(), "takeFocus");
    settle();

    // Panes.
    key(Qt::Key_Backslash, Qt::ControlModifier);
    QCOMPARE(m_workspace->paneCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);

    key('h', Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 0);
    key('l', Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 1);

    key('w', Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(m_workspace->paneCount(), 1);

    // Back to one tab, so the next test starts where this one found things.
    while (m_workspace->paneAtIndex(0)->tabCount() > 1)
        m_workspace->closeTab(0, m_workspace->paneAtIndex(0)->tabCount() - 1);
    settle();

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after the tab and pane keys");
}

// The two text boxes, opened and left by key. Escape has to undo and Enter has
// to commit, and both have to hand the keyboard back to the list -- a field you
// can type into and not get out of is the same trap as a panel you cannot
// leave.
void TestStudio::typesAPathAndAFilter()
{
    QObject *root = m_engine->rootObjects().first();

    key('l', Qt::ControlModifier);
    QCOMPARE(root->property("editingPath").toBool(), true);
    key(Qt::Key_Escape);
    QCOMPARE(root->property("editingPath").toBool(), false);
    QCOMPARE(cwd(), m_tree.path());

    // Typed, committed, and the window went there.
    key('l', Qt::ControlModifier);
    QMetaObject::invokeMethod(root, "finishPathEdit", Q_ARG(QVariant, QVariant(true)),
                              Q_ARG(QVariant, QVariant(m_tree.filePath(
                                        QStringLiteral("alpha")))));
    settle();
    QCOMPARE(cwd(), m_tree.filePath(QStringLiteral("alpha")));
    QCOMPARE(root->property("editingPath").toBool(), false);
    key('h');
    QCOMPARE(cwd(), m_tree.path());

    key('/');
    QCOMPARE(root->property("filtering").toBool(), true);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("filter"));

    for (char ch : QByteArray("one"))
        key(ch);
    QCOMPARE(tab()->filter(), QStringLiteral("one"));

    key(Qt::Key_Return);                        // keep it
    QCOMPARE(root->property("filtering").toBool(), false);
    QCOMPARE(tab()->filter(), QStringLiteral("one"));
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    key('/');
    key(Qt::Key_Escape);                        // clear it
    QCOMPARE(tab()->filter(), QString());
    QCOMPARE(root->property("filtering").toBool(), false);

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after the text boxes");
}

// The graph is a second view of the same index, and it used to be reachable
// only by mouse: its key handler could open the node under the selection but
// nothing could move the selection, so with no mouse there was never a node
// under it.
void TestStudio::walksTheGraphWithTheKeyboard()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("graph"));
    QVERIFY(root->property("activeGraph").value<QObject *>());

    QObject *graph = root->property("activeGraph").value<QObject *>();
    QCOMPARE(graph->property("selectedId").toString(), QString());

    key('j');
    const QString first = graph->property("selectedId").toString();
    QVERIFY2(!first.isEmpty(), "j did not put the selection on a node");

    key('j');
    QVERIFY(graph->property("selectedId").toString() != first);
    key('k');
    QCOMPARE(graph->property("selectedId").toString(), first);

    key('G');
    const QString last = graph->property("selectedId").toString();
    QVERIFY(last != first);
    key('g');
    QCOMPARE(graph->property("selectedId").toString(), first);

    // Activating a tag node lists the files carrying it: a files tab filtered
    // to that tag, which is what picking the same tag in the dock does to the
    // list beside it, and what a tag in the Places panel does in Dolphin or a
    // tag in the sidebar does in Finder. Written onto the graph tab instead, a
    // tag reaches nothing but that tab's title.
    //
    // A tag and not a file because this case is about tags -- the file half is
    // `recordsAnOpenInsteadOfLaunchingIt`, which can press it now that the
    // launcher is a seam. It used to be a tag because activating a file node
    // launched whatever the machine had registered for text/plain, and a suite
    // that does that is a suite nobody can run.
    graph->setProperty("selectedId", QString());
    QString picked;
    for (int i = 0; i < 32 && picked.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (id.startsWith(QLatin1String("tag")))
            picked = id;
    }
    QVERIFY2(!picked.isEmpty(), "the graph has no tag node to activate");

    key('l');
    QVERIFY2(tab()->kind() == QLatin1String("files"),
             "activating a tag node did not open a listing for it");
    const QString picked_tag = tab()->selectedTag();
    QVERIFY2(!picked_tag.isEmpty(), "the listing a tag node opened is filtered by nothing");
    // The graph draws tags as `#work`; the `#` is decoration and must not reach
    // the index, which has no tag by that name.
    QVERIFY2(!picked_tag.startsWith(QLatin1Char('#')),
             "the graph's # decoration leaked into the tag filter");
    QVERIFY(indexModelHasTag(m_index, picked_tag));
    // Filtered by it, not merely labelled with it: every file row carries the
    // tag. Folders are never filtered out, so they are not counted here.
    QObject *listed = filesRoot()->property("folder").value<QObject *>();
    QVERIFY(listed);
    int files = 0;
    for (int i = 0; i < listed->property("count").toInt(); ++i) {
        QString kind;
        QMetaObject::invokeMethod(listed, "kindAt", Q_RETURN_ARG(QString, kind),
                                  Q_ARG(int, i));
        if (kind != QLatin1String("file"))
            continue;
        ++files;
        QString path;
        QMetaObject::invokeMethod(listed, "pathAt", Q_RETURN_ARG(QString, path),
                                  Q_ARG(int, i));
        QVERIFY2(m_index->tagsForPath(path).contains(picked_tag),
                 "the listing a tag node opened holds a file without that tag");
    }
    QVERIFY2(files > 0, "the listing a tag node opened has no file in it to check");

    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("graph"));
    graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    // The named spellings of the same four moves. `j k g G` were pressed above
    // and the arrows, Home and End were not -- so the graph could have answered
    // only the letters and the suite would not have known.
    graph->setProperty("selectedId", QString());
    key(Qt::Key_Right);
    const QString stepped = graph->property("selectedId").toString();
    QVERIFY2(!stepped.isEmpty(), "Right did not put the selection on a node");
    key(Qt::Key_Right);
    QVERIFY(graph->property("selectedId").toString() != stepped);
    key(Qt::Key_Left);
    QCOMPARE(graph->property("selectedId").toString(), stepped);
    key(Qt::Key_End);
    const QString ended = graph->property("selectedId").toString();
    QVERIFY(ended != stepped);
    key(Qt::Key_Home);
    QVERIFY(graph->property("selectedId").toString() != ended);

    // Menu opens the row menu, the way it does in the list. A file node this
    // time: the menu is the one thing on the graph that is only for files.
    graph->setProperty("selectedId", QString());
    QString file;
    for (int i = 0; i < 32 && file.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (!id.isEmpty() && !id.startsWith(QLatin1String("tag")))
            file = id;
    }
    QVERIFY2(!file.isEmpty(), "the graph has no file node to open a menu on");
    key(Qt::Key_Menu);
    QVERIFY2(root->property("blocked").toBool(), "Menu opened nothing on the graph");
    key(Qt::Key_Escape);
    settle();

    // Enter activates, like `l`. A tag node again, for the same reason.
    graph->setProperty("selectedId", picked);
    settle();
    key(Qt::Key_Return);
    QCOMPARE(tab()->kind(), QStringLiteral("files"));
    QCOMPARE(tab()->selectedTag(), picked_tag);
    m_workspace->closeCurrentTab();
    settle();

    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(tab()->kind(), QStringLiteral("files"));
    QVERIFY2(root->property("activeGraph").value<QObject *>() == nullptr,
             "the closed graph tab is still the one the keyboard talks to");
}

// The palette is the way to reach anything that did not earn a key, so a
// palette that opens and does nothing is worse than no palette. Typed, chosen
// and the window did the thing.
void TestStudio::runsACommandFromThePalette()
{
    QObject *root = m_engine->rootObjects().first();
    QCOMPARE(tab()->viewMode(), QStringLiteral("list"));

    key(':');
    QCOMPARE(root->property("blocked").toBool(), true);

    // Deliberately spelled with `j` and `k` in it: the palette used to treat
    // those two letters as list navigation and accept the event, so a query
    // containing either lost the letter. `icons` alone has neither, which is
    // why nothing noticed.
    for (char ch : QByteArray("jkicons"))
        key(ch);
    // The two that were swallowed, and the whole word, arrived.
    QObject *query = root->findChild<QObject *>(QStringLiteral("paletteQuery"));
    QVERIFY2(query, "the palette has no query field to check");
    QCOMPARE(query->property("text").toString(), QStringLiteral("jkicons"));

    // With them typed the filter matches nothing, so clear and ask properly.
    for (int i = 0; i < 7; ++i)
        key(Qt::Key_Backspace);
    for (char ch : QByteArray("icons"))
        key(ch);
    key(Qt::Key_Return);

    QCOMPARE(tab()->viewMode(), QStringLiteral("icons"));
    QCOMPARE(root->property("blocked").toBool(), false);
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("list"));

    key('1');
    QCOMPARE(tab()->viewMode(), QStringLiteral("list"));

    // Opened and abandoned changes nothing and gives the keyboard back.
    key('p', Qt::ControlModifier);
    QCOMPARE(root->property("blocked").toBool(), true);
    key(Qt::Key_Escape);
    QCOMPARE(root->property("blocked").toBool(), false);

    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the list stopped answering after the palette");
}

// Palette sort commands call pickSortRole, so repeating one toggles direction
// instead of leaving the direction the previous field set.
void TestStudio::commandPaletteSortUsesPickSortRole()
{
    QCOMPARE(tab()->sortRole(), QStringLiteral("name"));
    QCOMPARE(tab()->sortAscending(), true);

    key(':');
    for (char ch : QByteArray("sort-mtime"))
        key(ch);
    key(Qt::Key_Return);

    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    QCOMPARE(tab()->sortAscending(), false);

    key(':');
    for (char ch : QByteArray("sort-mtime"))
        key(ch);
    key(Qt::Key_Return);

    QCOMPARE(tab()->sortRole(), QStringLiteral("mtime"));
    QCOMPARE(tab()->sortAscending(), true);
}

// `0`, `+` and `-` used to be three keys whose only effect was on pixels, so
// nothing could tell whether they worked. They are the reason GraphItem grew a
// readable `scale`.
void TestStudio::zoomsAndResetsTheGraph()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    QCOMPARE(graph->property("zoomLevel").toReal(), 1.0);

    // The change has to be announced, not only stored: a binding on zoomLevel
    // that never updates is a readout that lies, and nothing in the QML reads
    // it yet, so this is the only thing keeping the signal honest.
    QSignalSpy announced(graph, SIGNAL(viewChanged()));

    key('+');
    QVERIFY2(announced.count() >= 1, "zooming in did not announce the change");
    const qreal bigger = graph->property("zoomLevel").toReal();
    QVERIFY2(bigger > 1.0, "+ did not zoom in");

    key('-');
    QVERIFY2(graph->property("zoomLevel").toReal() < bigger, "- did not zoom out");

    // `=` is the unshifted `+` on most layouts, and has to zoom the same way.
    // Asserted against the value before the press: comparing it to 1.0 was true
    // whether or not the key did anything, because the two presses above had
    // already brought it back to 1.0.
    const qreal beforeEquals = graph->property("zoomLevel").toReal();
    key('=');
    QVERIFY2(graph->property("zoomLevel").toReal() > beforeEquals, "= did not zoom in");

    key('+');
    key('+');
    QVERIFY(graph->property("zoomLevel").toReal() != 1.0);
    const int announcedBeforeReset = announced.count();
    key('0');
    QCOMPARE(graph->property("zoomLevel").toReal(), 1.0);
    QVERIFY2(announced.count() > announcedBeforeReset,
             "resetting the view did not announce the change");

    // It stops rather than running away: the bounds are what keep a held key
    // from zooming to a blank screen.
    for (int i = 0; i < 40; ++i)
        key('+');
    QVERIFY(graph->property("zoomLevel").toReal() <= 3.0);
    for (int i = 0; i < 80; ++i)
        key('-');
    QVERIFY(graph->property("zoomLevel").toReal() >= 0.4);

    m_workspace->closeCurrentTab();
    settle();
}

// The whole of the graph's mouse handling was unreached by any test, including
// `hitNode` -- the calculation that decides which node you clicked on, and the
// one that had to change when the labels stopped being truncated.
void TestStudio::picksAGraphNodeWithTheMouse()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    auto *item = qobject_cast<QQuickItem *>(graph);
    QVERIFY(item);

    // The layout is computed from the item's size, so a node's position is only
    // meaningful once that size is final. Forcing a frame settles it; without
    // this the test read a position from one layout and clicked it in another,
    // and failed only when the machine was busy enough for the two to differ.
    QTest::qWait(80);
    window()->grabWindow();
    QTRY_VERIFY(item->width() > 100 && item->height() > 100);
    const QSizeF laidOutAt(item->width(), item->height());

    // Walk to a file node and ask it where it is: `m` reports the node's own
    // view coordinates, so the test needs no guess about the layout.
    QString picked;
    for (int i = 0; i < 64 && picked.isEmpty(); ++i) {
        key('j');
        const QString id = graph->property("selectedId").toString();
        if (id.startsWith(QLatin1String("file")))
            picked = id;
    }
    QVERIFY2(!picked.isEmpty(), "the graph has no file node to click on");

    QSignalSpy asked(graph, SIGNAL(nodeMenuRequested(QString, QString, QString,
                                                     qreal, qreal)));
    key('m');
    QVERIFY2(asked.count() == 1, "m reported no position for the selected node");
    const QPointF at(asked.at(0).at(3).toReal(), asked.at(0).at(4).toReal());
    key(Qt::Key_Escape);

    graph->setProperty("selectedId", QString());
    settle();

    // If the item resized between the two, the coordinates describe a layout
    // that no longer exists -- say that, rather than failing on the click.
    QCOMPARE(QSizeF(item->width(), item->height()), laidOutAt);

    const QPointF scene = item->mapToScene(at);
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, scene.toPoint());
    settle();
    QCOMPARE(graph->property("selectedId").toString(), picked);

    // A click on empty space pans without losing the selection.
    const qreal before = graph->property("zoomLevel").toReal();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, QPoint(4, 200));
    settle();
    QCOMPARE(graph->property("selectedId").toString(), picked);
    QCOMPARE(graph->property("zoomLevel").toReal(), before);

    m_workspace->closeCurrentTab();
    settle();
}

// Every view has to survive being drawn, and be the thing that drew.
//
// The first version of this counted colours across the whole window, which the
// toolbar, tab strip, dock and status bar satisfy on their own -- so replacing
// the graph's entire painter with a `return` left it passing. It now looks only
// at the region the view owns, and compares that region against the same window
// with the view empty.
void TestStudio::drawsEveryView()
{
    QObject *root = m_engine->rootObjects().first();

    // The pane's content area: below the header and tab strip, left of the
    // dock, above the status line. Generous margins, so it holds no chrome.
    const auto contentInk = [this] {
        settle();
        QTest::qWait(60);
        const QImage shot = window()->grabWindow();
        const QRect area(20, 110, qMax(10, shot.width() - 300 - 20),
                          qMax(10, shot.height() - 110 - 60));
        QSet<QRgb> seen;
        int marks = 0;
        const QRgb blank = shot.pixel(area.right() - 2, area.bottom() - 2);
        for (int y = area.top(); y < area.bottom(); y += 3) {
            for (int x = area.left(); x < area.right(); x += 3) {
                const QRgb pixel = shot.pixel(x, y);
                seen.insert(pixel);
                if (pixel != blank)
                    ++marks;
            }
        }
        return QPair<int, int>(seen.size(), marks);
    };

    const auto drew = [&](const char *what, int leastColours, int leastMarks) {
        const QPair<int, int> ink = contentInk();
        QVERIFY2(ink.first >= leastColours && ink.second >= leastMarks,
                 qPrintable(QStringLiteral("%1: %2 colours, %3 marked pixels")
                                .arg(QString::fromLatin1(what))
                                .arg(ink.first)
                                .arg(ink.second)));
    };

    focusRow(QStringLiteral("two.md"));
    drew("the list view drew nothing", 4, 400);
    key('2');
    drew("the details view drew nothing", 4, 400);
    key('3');
    drew("the icons view drew nothing", 4, 400);
    key('1');

    // An empty folder is the floor: whatever the chrome contributes to that
    // rectangle, a drawn view has to beat it.
    tab()->setCwd(m_tree.filePath(QStringLiteral("beta")));
    settle();
    const QPair<int, int> bare = contentInk();
    tab()->setCwd(m_tree.path());
    settle();
    const QPair<int, int> full = contentInk();
    QVERIFY2(full.second > bare.second * 2,
             qPrintable(QStringLiteral("a full folder marked %1 pixels, an empty one %2")
                            .arg(full.second).arg(bare.second)));

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);
    const QPair<int, int> drawn = contentInk();

    // The graph, against itself with nothing in it. A painter that returned
    // early would land on the same number as an empty index.
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
    index.close();
    m_index->reload();
    const QPair<int, int> empty = contentInk();

    QVERIFY2(drawn.second > empty.second + 200,
             qPrintable(QStringLiteral("the graph marked %1 pixels; with no index at "
                                       "all it marked %2")
                            .arg(drawn.second).arg(empty.second)));
    QVERIFY2(drawn.first > empty.first,
             "the graph drew no colour the empty one did not");

    writeIndex();
    m_index->reload();
    m_workspace->closeCurrentTab();
    settle();
}

// Clicking a file in the other pane has to make that pane the focused one.
//
// The only thing that set `focusedPane` from the mouse sat at `z: -1` beneath
// the whole pane, and the list took the press first -- so a click moved that
// pane's cursor while the keyboard went on driving the other one, and the row
// you had just clicked was not the row `j` would move from.
void TestStudio::clickingARowFocusesItsPane()
{
    QMetaObject::invokeMethod(m_workspace, "splitRight",
                              Q_ARG(QString, QStringLiteral("files")));
    settle();
    QCOMPARE(m_workspace->paneCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);

    // Aimed at the pane itself, not at a fraction of the window. The split is
    // laid out on a later frame than the one `settle` pumps, so a hardcoded
    // point could be measured against a layout that had not happened yet --
    // which is why this failed about one run in ten, and only under load.
    QTest::qWait(80);
    window()->grabWindow();
    const auto panes = [this] { return itemsNamed(QStringLiteral("pane")); };
    QTRY_COMPARE(panes().size(), 2);
    QQuickItem *left = nullptr;
    for (QQuickItem *item : panes()) {
        if (!left || item->mapToScene(QPointF(0, 0)).x() < left->mapToScene(QPointF(0, 0)).x())
            left = item;
    }
    QVERIFY(left);
    QTRY_VERIFY2(left->width() > 80 && left->height() > 80,
                 "the split had not been laid out");
    const QPointF onTheLeft = left->mapToScene(QPointF(left->width() * 0.25,
                                                       left->height() * 0.35));
    // Put the cursor somewhere the click is not, so "the click moved it" is a
    // fact about the click.
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(0)));
    settle();
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, onTheLeft.toPoint());
    settle();
    QCOMPARE(m_workspace->focusedPane(), 0);

    // And the click reached the row, not only the pane under it. The pane's
    // catch-all declines the press so it goes on to the list; a catch-all that
    // took the press instead left the focus right and the row unselected, and
    // this test -- the one named for clicking a row -- could not tell.
    QVERIFY(filesRoot());
    QVERIFY2(cursor() != 0, "the click focused the pane but did not select a row");

    // And the keyboard now drives the pane that was clicked.
    const int before = cursor();
    key('j');
    QVERIFY2(cursor() != before, "the keyboard did not follow the click");

    // And empty space inside a pane counts as that pane. The rows carry a tap
    // handler of their own; below the last row there is nothing but the pane,
    // and a click there used to move the keyboard nowhere -- so the pane you
    // were looking at and the pane `j` drove were different ones.
    m_workspace->setFocusedPane(1);
    settle();
    QCOMPARE(m_workspace->focusedPane(), 1);
    m_workspace->setFocusedPane(1);
    settle();
    QCOMPARE(m_workspace->focusedPane(), 1);
    const QPointF onEmptySpace = left->mapToScene(QPointF(left->width() * 0.25,
                                                          left->height() * 0.9));
    QTest::mouseClick(window(), Qt::LeftButton, Qt::NoModifier, onEmptySpace.toPoint());
    settle();
    QCOMPARE(m_workspace->focusedPane(), 0);

    m_workspace->closePane(1);
    settle();
    QCOMPARE(m_workspace->paneCount(), 1);
}

// A palette on disk, the shape omarchy writes one: `colors.toml` under the
// theme the desktop currently points at.
static bool writePalette(const QString &root, const char *background, const char *foreground,
                          const char *accent, const char *muted)
{
    const QString dir = root + QStringLiteral("/omarchy/current/theme");
    if (!QDir().mkpath(dir))
        return false;
    QFile file(QDir(dir).filePath(QStringLiteral("colors.toml")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    file.write(QStringLiteral("mode = \"dark\"\n"
                               "background = \"%1\"\nforeground = \"%2\"\n"
                               "accent = \"%3\"\nred = \"%3\"\nmuted = \"%4\"\n")
                   .arg(QString::fromLatin1(background), QString::fromLatin1(foreground),
                        QString::fromLatin1(accent), QString::fromLatin1(muted))
                   .toUtf8());
    return true;
}

// The state directory for as long as it is in scope, so a `Theme` built inside
// a test reads the palette that test wrote and the window keeps the fixture's.
struct StateEnv {
    explicit StateEnv(const QString &root)
        : previous(qgetenv("XDG_STATE_HOME"))
    {
        qputenv("XDG_STATE_HOME", root.toUtf8());
    }
    ~StateEnv() { qputenv("XDG_STATE_HOME", previous); }
    const QByteArray previous;
};

// The graph wears the desktop's theme like everything else.
//
// It did not. `GraphItem` declares a `theme` property of its own, and a QML
// binding searches the object before the context -- so `theme: theme` on that
// one type bound the property to itself, left it null forever, and the painter
// fell back to hardcoded colours. Every other panel followed the desktop, so
// nothing looked broken; the graph was simply always the same blue.
void TestStudio::dressesTheGraphInTheDesktopTheme()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();

    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    QObject *dressed = graph->property("theme").value<QObject *>();
    QVERIFY2(dressed, "the graph has no theme, so it draws in hardcoded colours");
    QCOMPARE(dressed, static_cast<QObject *>(m_theme));

    // And it draws in it. Holding the theme and filling every node from a
    // literal is the same picture on every desktop, and the property above
    // cannot tell the difference -- so this paints the map twice, once per
    // palette, and reads the pixels.
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());
    const StateEnv scope(desktop.path());

    const auto drawGraph = [](Theme *theme, IndexModel *index) {
        GraphItem item;
        item.setWidth(900);
        item.setHeight(600);
        item.setTheme(theme);
        item.setIndex(index);
        QImage canvas(900, 600, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(theme->background());
        QPainter painter(&canvas);
        item.paint(&painter);
        return canvas;
    };

    // On a desktop of one hue every colour on the map is that hue. A node
    // filled from a literal comes out on another channel, and a line drawn in
    // translucent white comes out grey, whatever the desktop is wearing.
    const auto offPalette = [](const QImage &shot, int channel) {
        int wrong = 0;
        for (int y = 0; y < shot.height(); y += 2) {
            for (int x = 0; x < shot.width(); x += 2) {
                const QColor pixel = shot.pixelColor(x, y);
                const int most = qMax(pixel.red(), qMax(pixel.green(), pixel.blue()));
                const int least = qMin(pixel.red(), qMin(pixel.green(), pixel.blue()));
                // Dark enough to be the background rather than a mark on it.
                if (most <= 24)
                    continue;
                if (most - least <= 6 || (channel == 0 ? pixel.red() : pixel.blue()) < most)
                    ++wrong;
            }
        }
        return wrong;
    };

    QVERIFY(writePalette(desktop.path(), "#000000", "#ff6040", "#ff2020", "#a03030"));
    Theme red;
    const QImage onRed = drawGraph(&red, m_index);
    QVERIFY(writePalette(desktop.path(), "#000000", "#4060ff", "#2020ff", "#3040a0"));
    Theme blue;
    const QImage onBlue = drawGraph(&blue, m_index);

    const int redStrays = offPalette(onRed, 0);
    QVERIFY2(redStrays == 0,
             qPrintable(QStringLiteral("%1 pixels of the map are not the red desktop's")
                            .arg(redStrays)));
    const int blueStrays = offPalette(onBlue, 2);
    QVERIFY2(blueStrays == 0,
             qPrintable(QStringLiteral("%1 pixels of the map are not the blue desktop's")
                            .arg(blueStrays)));
    QVERIFY2(onRed != onBlue, "two desktops, one picture");

    // The line it prints when there is nothing to draw is the desktop's too.
    const int emptyStrays = offPalette(drawGraph(&red, nullptr), 0);
    QVERIFY2(emptyStrays == 0,
             qPrintable(QStringLiteral("%1 pixels of the empty map are not the desktop's")
                            .arg(emptyStrays)));

    m_workspace->closeCurrentTab();
    settle();
}

// A map of part of the index says which part.
//
// The view carries as many files as a ring of names can be read at, and an
// index larger than that is drawn short. Stopping quietly makes the picture a
// claim about a collection it is not showing, so the count of what it drew and
// the count of what there is both go on the status line -- the same rows
// `omafiles tags` answers with.
void TestStudio::saysHowMuchOfTheIndexTheGraphDrew()
{
    QObject *root = m_engine->rootObjects().first();
    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    // The fixture fits, so there is nothing to leave out and nothing to say.
    QCOMPARE(graph->property("summary").toString(), QStringLiteral("7 tags · 7 files"));

    QTemporaryDir many;
    QVERIFY(many.isValid());
    const QDir dir(many.path());
    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    const int total = 400;
    for (int i = 0; i < total; ++i) {
        const QString path = dir.filePath(QStringLiteral("file%1.txt").arg(i));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"many\"]}\n").arg(path).toUtf8());
    }
    index.close();
    m_index->reload();
    settle();

    const int drawn = graph->property("nodeCount").toInt() - 1;   // the one tag
    QVERIFY2(drawn >= 300,
             qPrintable(QStringLiteral("the map carries %1 files of %2").arg(drawn).arg(total)));
    QVERIFY2(drawn < total, "nothing was left out, so there is nothing to check");
    QCOMPARE(graph->property("summary").toString(),
             QStringLiteral("1 tags · %1 of %2 files").arg(drawn).arg(total));

    writeIndex();
    m_index->reload();
    settle();
    QCOMPARE(graph->property("summary").toString(), QStringLiteral("7 tags · 7 files"));

    m_workspace->closeCurrentTab();
    settle();
}

// The status line on a graph tab is about the graph.
//
// It reads the pane's `FolderModel`, and a graph tab does not have one: the
// counts on the left are then a confident `0/0` and `0 tagged here` about an
// index with seven tagged files in it. A view says where you are in it in the
// terms it has -- the node under the cursor, of the nodes there are.
void TestStudio::saysWhereTheCursorIsOnAGraphTab()
{
    QObject *root = m_engine->rootObjects().first();
    QObject *status = root->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(status);
    QObject *note = root->findChild<QObject *>(QStringLiteral("statusNote"));
    QVERIFY(note);
    const int inTheFolder = status->property("total").toInt();
    QVERIFY(inTheFolder > 0);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    QObject *graph = root->property("activeGraph").value<QObject *>();
    QVERIFY(graph);

    const int nodes = graph->property("nodeCount").toInt();
    QVERIFY(nodes > 0);
    QCOMPARE(status->property("total").toInt(), nodes);
    QCOMPARE(status->property("cursor").toInt(), -1);
    QCOMPARE(note->property("text").toString(), graph->property("summary").toString());

    key('j');
    QCOMPARE(graph->property("selectedRow").toInt(), 0);
    QCOMPARE(status->property("cursor").toInt(), 0);
    key('j');
    QCOMPARE(status->property("cursor").toInt(), 1);

    // And the folder gets its own line back when the folder is what is in front
    // of you.
    m_workspace->closeCurrentTab();
    settle();
    QCOMPARE(status->property("total").toInt(), inTheFolder);
    QCOMPARE(note->property("text").toString(),
             QStringLiteral("%1 tagged here").arg(status->property("tagged").toInt()));
}

// Every corner in the window is the desktop's, and the desktop does not keep
// that number with its colours.
//
// `colors.toml` is colours only. hyprland's `decoration.rounding` is what every
// `Theme.rounding` binding in the window follows, so that is the file the theme
// reads: the user's override first, omarchy's default behind it.
void TestStudio::readsTheCornerRoundingFromTheHyprlandConfig()
{
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());
    QVERIFY(writeLooknfeel(desktop.filePath("config/hypr/looknfeel.lua"),
                           looknfeelRounding(12)));
    QVERIFY(writeLooknfeel(desktop.filePath("omarchy/default/hypr/looknfeel.lua"),
                           looknfeelRounding(3)));

    const DesktopEnv scope(desktop.path());
    Theme theme;
    QCOMPARE(theme.rounding(), 12);
}

// The override omarchy ships is the whole config commented out, this machine
// included -- so `rounding = 8` is sitting in it as a suggestion, behind a
// `--`, on every desktop that has never been customised. Read naively that is
// a window with round corners on a desktop with square ones.
void TestStudio::ignoresARoundingInsideALuaComment()
{
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());
    QVERIFY(writeLooknfeel(desktop.filePath("config/hypr/looknfeel.lua"),
                           commentedLooknfeel()));
    QVERIFY(writeLooknfeel(desktop.filePath("omarchy/default/hypr/looknfeel.lua"),
                           looknfeelRounding(3)));

    const DesktopEnv scope(desktop.path());
    Theme theme;
    QVERIFY2(theme.rounding() != 8, "read a rounding out of a Lua comment");
    // And a file that exists but says nothing on the subject falls through to
    // the default rather than to zero.
    QCOMPARE(theme.rounding(), 3);
}

// A desktop that is not omarchy has neither file, which is square corners and
// nothing else.
void TestStudio::roundsNothingWithoutAHyprlandConfig()
{
    QTemporaryDir desktop;
    QVERIFY(desktop.isValid());

    const DesktopEnv scope(desktop.path());
    Theme theme;
    QCOMPARE(theme.rounding(), 0);
}

// And it reaches the window, live.
//
// The getter alone is worth little: `rounding` is an integer with a default, so
// a theme that opened no file at all still answers a plausible number and
// agrees with itself. What is worth asserting is the radius of something on
// screen, before and after the desktop changes its mind.
void TestStudio::followsTheDesktopsRoundingIntoQml()
{
    QObject *root = m_engine->rootObjects().first();
    const QString looknfeel = m_desktop.filePath("config/hypr/looknfeel.lua");

    // The palette's query box, which is the one named item in the window whose
    // frame is drawn with `radius: Theme.rounding`.
    key(':');
    QCOMPARE(root->property("blocked").toBool(), true);
    QQuickItem *query =
        qobject_cast<QQuickItem *>(root->findChild<QObject *>(QStringLiteral("paletteQuery")));
    QVERIFY(query);
    QQuickItem *frame = query->parentItem();
    QVERIFY(frame);
    QCOMPARE(m_theme->rounding(), 0);
    QCOMPARE(frame->property("radius").toInt(), 0);

    QVERIFY(writeLooknfeel(looknfeel, looknfeelRounding(7)));
    QTRY_COMPARE(m_theme->rounding(), 7);
    settle();
    QCOMPARE(frame->property("radius").toInt(), 7);

    // Back to square, so the rest of the suite and the documentation shots see
    // the window the fixture describes.
    QVERIFY(writeLooknfeel(looknfeel, commentedLooknfeel()));
    QTRY_COMPARE(m_theme->rounding(), 0);
    settle();
    QCOMPARE(frame->property("radius").toInt(), 0);

    key(Qt::Key_Escape);
    QCOMPARE(root->property("blocked").toBool(), false);
}

// `?` and `Space`, which fell between the two keymap tests.
//
// The sweep exempts `?` because it opens a sheet that takes the keyboard, and
// skips `Space` because its spelling is a word -- so the sheet the whole keymap
// test is built around, and the key that marks a row, were pressed by nothing.
void TestStudio::showsTheKeySheetAndMarksARow()
{
    QObject *root = m_engine->rootObjects().first();

    QCOMPARE(root->property("blocked").toBool(), false);
    key('?');
    QVERIFY2(root->property("blocked").toBool(), "? did not open the key sheet");
    // Any key closes it, which is the sheet's own promise.
    key('j');
    QCOMPARE(root->property("blocked").toBool(), false);

    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QMetaObject::invokeMethod(folder, "clearMarks");
    QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(1)));
    settle();
    QCOMPARE(folder->property("markedCount").toInt(), 0);

    key(Qt::Key_Space);
    QCOMPARE(folder->property("markedCount").toInt(), 1);
    key(Qt::Key_Space);
    QCOMPARE(folder->property("markedCount").toInt(), 0);

    // And in the tag panel, where it toggles the tag under the cursor.
    key('t');
    QCOMPARE(root->property("focusRegion").toString(), QStringLiteral("tags"));
    QCOMPARE(tab()->selectedTag(), QString());
    key(Qt::Key_Space);
    QVERIFY2(!tab()->selectedTag().isEmpty(), "Space picked no tag in the tag panel");
    key(Qt::Key_Space);
    QCOMPARE(tab()->selectedTag(), QString());
    key(Qt::Key_Escape);
    settle();

    // Every entry on the sheet has to be readable. Widening it to four columns
    // stopped the last group falling off the bottom and started cutting the
    // longest label off at the right instead -- the sort entry read `name,
    // tags, type, size, mo...` -- which is the same failure in the other
    // direction, and the committed screenshot had it in it.
    key('?');
    QVERIFY(root->property("blocked").toBool());
    window()->grabWindow();
    settle();
    const QList<QQuickItem *> entries = itemsNamed(QStringLiteral("helpEntry"));
    QVERIFY2(entries.size() >= 20, "the key sheet rendered almost nothing");
    for (QQuickItem *entry : entries) {
        const QString text = entry->property("text").toString();
        QVERIFY2(!entry->property("truncated").toBool(),
                 qPrintable(QStringLiteral("the key sheet cuts off `%1`").arg(text)));
        // `truncated` is false for text that simply spills past its box, so it
        // catches eliding and not overflowing. Dropping the wrap and keeping
        // ElideNone would look fine to the assertion above and print the label
        // over its neighbour.
        QVERIFY2(entry->property("contentWidth").toReal() <= entry->width() + 0.5,
                 qPrintable(QStringLiteral("`%1` is wider than its column").arg(text)));
    }
    key(Qt::Key_Escape);
    settle();
}

// Closing a pane must not leave a binding running against the pane it closed.
//
// `pragma ComponentBehavior: Bound` keeps the enclosing scope alive for one
// more evaluation, so the tab delegate re-ran with its pane already null and
// printed a TypeError on every close. Log noise today, and the noise that hides
// the next real one.
void TestStudio::survivesClosingAPaneWithoutComplaint()
{
    QStringList complaints;
    const auto previous = qInstallMessageHandler(nullptr);
    static QStringList *sink = nullptr;
    sink = &complaints;
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &text) {
        if (sink && (type == QtWarningMsg || type == QtCriticalMsg))
            sink->append(text);
    });

    QMetaObject::invokeMethod(m_workspace, "splitRight",
                              Q_ARG(QString, QStringLiteral("files")));
    settle();
    QCOMPARE(m_workspace->paneCount(), 2);
    m_workspace->closePane(1);
    settle();
    QCOMPARE(m_workspace->paneCount(), 1);

    // The binding does not re-evaluate until the scene is rendered again, and
    // `settle` only spins the event loop. Handing the handler back after two
    // rounds of processEvents captured nothing at all on any run -- so the
    // assertion below was true of an empty list whether the fix was there or
    // not. Force the render while still listening.
    window()->grabWindow();
    QTest::qWait(60);
    settle();
    window()->grabWindow();
    settle();

    sink = nullptr;
    qInstallMessageHandler(previous);

    const QStringList errors = complaints.filter(QStringLiteral("TypeError"));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
}

// A tag added by the CLI shows up without anyone pressing reload.
//
// The window watches the index file, and several tests here quietly depend on
// that -- the reload keys are asserted by the signal they emit rather than by
// what arrives, precisely because the watcher has already fetched it. Nothing
// asserted the watcher itself. It also coalesces now, because one CLI write is
// four filesystem events -- a lock file appearing, a temporary appearing, the
// rename, the lock file going -- and each one used to cost a full re-read; a
// coalescer that swallowed the last event would be silent in exactly the same
// way as no watcher at all.
void TestStudio::noticesTheIndexChangingUnderIt()
{
    const QString index = QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl"));
    const int before = m_index->tagRowCount();
    QVERIFY(before > 0);

    // Appended in place, not committed by rename the way the CLI does. That is
    // deliberate and it is enough: the watch is on the file, and either shape
    // of write moves it. The rename path is covered by the CLI suite.
    {
        QFile handle(index);
        QVERIFY(handle.open(QIODevice::Append | QIODevice::Text));
        const QString path = QDir(m_tree.path()).filePath(QStringLiteral("one.txt"));
        handle.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"watched\"]}\n")
                         .arg(path).toUtf8());
    }

    QTRY_VERIFY_WITH_TIMEOUT(indexModelHasTag(m_index, QStringLiteral("watched")), 4000);
    QVERIFY(m_index->tagRowCount() > before);

    // And put the fixture back, so the tests after this one see what they
    // were written against.
    writeIndex();
    QTRY_VERIFY_WITH_TIMEOUT(!indexModelHasTag(m_index, QStringLiteral("watched")), 4000);
    QTRY_COMPARE(m_index->tagRowCount(), before);
}

// On a machine where the command line has never run there is no index
// directory, so there was nothing to watch and nothing ever took the watch
// again -- the window's promise that it follows the file was false until
// somebody pressed F5, which is exactly the first run where nobody knows to.
void TestStudio::watchesTheIndexOnAFreshInstall()
{
    QTemporaryDir fresh;
    QVERIFY(fresh.isValid());
    const QByteArray previous = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", fresh.path().toUtf8());

    {
        IndexModel model;
        const QDir data(QDir(fresh.path()).filePath(QStringLiteral("omafiles")));
        QVERIFY2(data.exists(), "the studio did not make the directory it means to watch");
        QCOMPARE(model.tagRowCount(), 0);

        QFile index(data.filePath(QStringLiteral("index.jsonl")));
        QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Text));
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[\"firstrun\"]}\n")
                        .arg(QDir(m_tree.path()).filePath(QStringLiteral("one.txt")))
                        .toUtf8());
        index.close();

        QTRY_VERIFY_WITH_TIMEOUT(indexModelHasTag(&model, QStringLiteral("firstrun")), 4000);
    }

    qputenv("XDG_DATA_HOME", previous);
}

// The keys the bare-character sweep cannot reach.
//
// It skips anything whose spelling has more than one character, which is every
// named key and every chord -- so `Home`, `End`, `PgUp`, `PgDn`, `Ctrl+D`,
// `Ctrl+U`, `F5`, `Ctrl+R`, `Menu`, `Shift+F10` and the arrows were on the key
// sheet and pressed by nothing. Four of them had just been added to that sheet
// in the belief they were covered.
void TestStudio::answersTheNamedAndChordedKeys()
{
    QObject *root = m_engine->rootObjects().first();
    PaneState *pane = m_workspace->paneAtIndex(0);
    const QString sweep = m_tree.filePath(QStringLiteral("sweep"));

    const auto reset = [&](int row) {
        while (pane->tabCount() > 1)
            m_workspace->closeTab(0, pane->tabCount() - 1);
        pane->setCurrentIndex(0);
        tab()->setCwd(sweep);
        tab()->setViewMode(QStringLiteral("list"));
        QMetaObject::invokeMethod(root, "takeFocus");
        settle();
        QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(row)));
        settle();
    };

    // Movement.
    reset(1);
    key(Qt::Key_Down);
    QCOMPARE(cursor(), 2);
    key(Qt::Key_Up);
    QCOMPARE(cursor(), 1);
    key(Qt::Key_End);
    QCOMPARE(cursor(), filesRoot()->property("folder").value<QObject *>()
                           ->property("count").toInt() - 1);
    key(Qt::Key_Home);
    QCOMPARE(cursor(), 0);

    reset(0);
    key(Qt::Key_PageDown);
    QVERIFY2(cursor() > 0, "PgDn did not move the cursor");
    key(Qt::Key_PageUp);
    QCOMPARE(cursor(), 0);

    reset(0);
    key(Qt::Key_D, Qt::ControlModifier);
    const int half = cursor();
    QVERIFY2(half > 0, "Ctrl+D did not move the cursor");
    key(Qt::Key_U, Qt::ControlModifier);
    QVERIFY2(cursor() < half, "Ctrl+U did not move the cursor back");

    // Walking the tree with the named keys.
    reset(1);
    key(Qt::Key_Right);
    QVERIFY2(cwd() != sweep, "Right did not enter the folder under the cursor");
    key(Qt::Key_Left);
    QCOMPARE(cwd(), sweep);
    reset(1);
    key(Qt::Key_Backspace);
    QVERIFY2(cwd() != sweep, "Backspace did not go up");

    // The menu, by both of its named spellings.
    for (const auto &press : {QPair<Qt::Key, Qt::KeyboardModifiers>(Qt::Key_Menu,
                                                                    Qt::NoModifier),
                              QPair<Qt::Key, Qt::KeyboardModifiers>(Qt::Key_F10,
                                                                    Qt::ShiftModifier)}) {
        reset(1);
        key(press.first, press.second);
        QVERIFY2(root->property("blocked").toBool(),
                 "a named menu key did not open the menu");
        key(Qt::Key_Escape);
        settle();
    }

    // Reload, by both spellings.
    //
    // Not observed through the index's contents: the window watches the file
    // and fetches a change on its own, so a row written behind its back is
    // already there by the time the key is pressed. What the key has to do is
    // ask -- so the test presses it with nothing changed and requires the ask.
    for (int i = 0; i < 2; ++i) {
        reset(1);
        QSignalSpy asked(m_index, SIGNAL(changed()));
        if (i == 0)
            key(Qt::Key_F5);
        else
            key(Qt::Key_R, Qt::ControlModifier);
        settle();
        QVERIFY2(asked.count() >= 1,
                 i == 0 ? "F5 did not reload the index"
                        : "Ctrl+R did not reload the index");
    }

    // Panes, by the arrow spelling the sweep never reaches.
    reset(1);
    key(Qt::Key_Backslash, Qt::ControlModifier);
    QCOMPARE(m_workspace->paneCount(), 2);
    QCOMPARE(m_workspace->focusedPane(), 1);
    key(Qt::Key_Left, Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 0);
    key(Qt::Key_Right, Qt::AltModifier);
    QCOMPARE(m_workspace->focusedPane(), 1);
    key(Qt::Key_W, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(m_workspace->paneCount(), 1);

    // The arrows in the icons view. It is the only entry on the sheet that
    // names a view, the bare-key sweep skips it because `\u2190 \u2192` is not ASCII,
    // and every other press of Left and Right here is in the list -- where they
    // walk the tree instead. So the one thing the entry promises was tested by
    // nothing.
    reset(1);
    tab()->setViewMode(QStringLiteral("icons"));
    settle();
    {
        const int before = cursor();
        key(Qt::Key_Right);
        QVERIFY2(cursor() > before, "Right did not move to the next icon");
        key(Qt::Key_Left);
        QCOMPARE(cursor(), before);
        QVERIFY2(cwd() == sweep, "an arrow in the icons view walked the tree");
    }
    tab()->setViewMode(QStringLiteral("list"));
    settle();

    // Enter, which the sheet spells as a word.
    reset(1);
    key(Qt::Key_Return);
    QVERIFY2(cwd() != sweep, "Enter did not enter the folder under the cursor");

    reset(0);
    tab()->setCwd(m_tree.path());
    settle();
}

// The sheet under `?` is the window's own claim about what it answers. Every
// single-character key on it is pressed here, and the window has to be alive
// after each one -- a documented key that hangs the keyboard is worse than an
// undocumented one.
void TestStudio::everyDocumentedKeyIsAnswered()
{
    QObject *root = m_engine->rootObjects().first();
    PaneState *pane = m_workspace->paneAtIndex(0);

    // What each bare key needs before it can show that it did anything. Several
    // are honest no-ops from a standing start -- `A` with nothing marked, `1`
    // already in the list view, `]` with one tab, and `g`, which is half of
    // `gg` -- and a sweep that pressed them cold would be asserting that
    // nothing happened.
    struct Case { const char *key; const char *setup; };
    static const Case cases[] = {
        {"j", ""},        {"k", ""},        {"l", ""},        {"h", ""},
        {"~", ""},        {"t", ""},        {"m", ""},        {"s", ""},
        {"S", ""},        {"G", ""},        {"a", ""},        {".", ""},
        {"g", "twice"},                    // one `g` only arms the second
        {"A", "mark"},                     // needs something to clear
        {"1", "icons"},   {"2", "list"},    {"3", "list"},
        {"]", "tabs"},    {"[", "tabs"},
        {"/", ""},                         // opens the filter bar
    };

    // The other half of the sheet: the rows spelled as words. The sweep below
    // cannot press those -- `Ctrl d` is one chord written as two words, `Bksp`
    // is a name -- so they are driven by `answersTheNamedAndChordedKeys` and by
    // the region tests, and what this list does is make the author say where.
    //
    // Weaker than the sweep, and worth having: without it a row spelled in more
    // than one character goes onto the sheet with no test asking about it, which
    // is how `Ctrl Tab` came to claim two moves for one chord.
    static const char *named[] = {
        "↓ ↑", "Ctrl d", "Ctrl u", "PgDn PgUp", "Home End", "← →",
        "Enter", "→", "←", "Bksp", "Ctrl l", "F5", "Ctrl r",
        "Space", "Menu", "Shift F10",
        "Ctrl h",
        "Esc", "← Esc",
        // worksTabsAndPanesFromTheKeyboard
        "Ctrl t", "Ctrl w", "Delete", "Ctrl Tab", "Ctrl Shift Tab", "Alt 1…9",
        "Ctrl \\", "Ctrl Shift w", "Alt h l", "Alt ← →",
        "Ctrl p", "Tab",
    };

    // The keymap drives the list, so a key added to the sheet without a case
    // here fails rather than going unchecked.
    QSet<QString> covered;
    for (const Case &one : cases)
        covered.insert(QString::fromLatin1(one.key));
    QSet<QString> driven;
    for (const char *one : named)
        driven.insert(QString::fromUtf8(one));

    const QVariantList groups = root->property("keymap").toList();
    QVERIFY2(!groups.isEmpty(), "the window has no keymap to check against");
    for (const QVariant &group : groups) {
        const QMap<QString, QVariant> section = group.toMap();
        // The graph tab answers its own keys and has its own test; sweeping
        // them in a files tab would only prove they do nothing there.
        if (section.value(QStringLiteral("title")).toString()
                .contains(QLatin1String("graph")))
            continue;

        for (const QVariant &entry : section.value(QStringLiteral("keys")).toList()) {
            const QStringList parts = entry.toMap().value(QStringLiteral("key"))
                                          .toString().split(QLatin1Char(' '));
            // One spelling, several keys: `j k` is two bare keys, `Ctrl d` is
            // one chord. A part longer than a character names a modifier or a
            // named key; those are driven by answersTheNamedAndChordedKeys.
            bool bare = true;
            for (const QString &part : parts) {
                if (part.size() != 1 || part.at(0).unicode() >= 128
                    || !part.at(0).isPrint())
                    bare = false;
            }
            if (!bare) {
                const QString spelling = entry.toMap().value(QStringLiteral("key")).toString();
                QVERIFY2(driven.contains(spelling),
                         qPrintable(QStringLiteral("`%1` is on the key sheet and no test "
                                                   "says which case drives it")
                                        .arg(spelling)));
                continue;
            }

            for (const QString &part : parts) {
                // `:` and `?` open sheets that take the keyboard; drawsItself
                // drives those.
                if (part == QLatin1String(":") || part == QLatin1String("?"))
                    continue;
                QVERIFY2(covered.contains(part),
                         qPrintable(QStringLiteral("`%1` is on the key sheet and this "
                                                   "test does not say what it should do")
                                        .arg(part)));
            }
        }
    }

    const QString sweep = m_tree.filePath(QStringLiteral("sweep"));

    for (const Case &one : cases) {
        const QString setup = QString::fromLatin1(one.setup);

        // A known place with somewhere to go in every direction.
        while (pane->tabCount() > 1)
            m_workspace->closeTab(0, pane->tabCount() - 1);
        pane->setCurrentIndex(0);
        tab()->setCwd(sweep);
        tab()->setViewMode(setup == QLatin1String("icons") ? QStringLiteral("icons")
                                                            : QStringLiteral("list"));
        tab()->setSortRole(QStringLiteral("name"));
        tab()->setSortAscending(true);
        tab()->setSelectedTag(QString());
        tab()->setShowHidden(false);
        QMetaObject::invokeMethod(root, "takeFocus");
        settle();

        QObject *folder = filesRoot()->property("folder").value<QObject *>();
        QMetaObject::invokeMethod(folder, "clearMarks");
        if (setup == QLatin1String("mark"))
            QMetaObject::invokeMethod(folder, "toggleMarkAt", Q_ARG(int, 2));
        if (setup == QLatin1String("tabs"))
            m_workspace->addTab(QStringLiteral("files"));
        QMetaObject::invokeMethod(filesRoot(), "setCursor", Q_ARG(QVariant, QVariant(1)));
        settle();

        const QString cwdBefore = cwd();
        const QString viewBefore = tab()->viewMode();
        const bool hiddenBefore = tab()->showHidden();
        const QString sortBefore = tab()->sortRole();
        const bool ascBefore = tab()->sortAscending();
        const QString regionBefore = root->property("focusRegion").toString();
        const bool blockedBefore = root->property("blocked").toBool();
        const int cursorBefore = cursor();
        const int markedBefore = folder->property("markedCount").toInt();
        const int tabBefore = pane->currentIndex();

        key(one.key[0]);
        if (setup == QLatin1String("twice"))
            key(one.key[0]);

        QObject *now = filesRoot() ? filesRoot()->property("folder").value<QObject *>()
                                    : nullptr;
        const bool answered = cursor() != cursorBefore
            || cwd() != cwdBefore
            || tab()->viewMode() != viewBefore
            || tab()->showHidden() != hiddenBefore
            || tab()->sortRole() != sortBefore
            || tab()->sortAscending() != ascBefore
            || root->property("focusRegion").toString() != regionBefore
            || root->property("blocked").toBool() != blockedBefore
            || pane->currentIndex() != tabBefore
            || (now && now->property("markedCount").toInt() != markedBefore);

        QVERIFY2(answered,
                 qPrintable(QStringLiteral("`%1` is on the key sheet and changed nothing "
                                           "the window can be asked about")
                                .arg(QChar::fromLatin1(one.key[0]))));

        key(Qt::Key_Escape);
        settle();
    }

    while (pane->tabCount() > 1)
        m_workspace->closeTab(0, pane->tabCount() - 1);
    tab()->setCwd(m_tree.path());
    tab()->setShowHidden(false);
    QMetaObject::invokeMethod(root, "takeFocus");
    settle();
}

void TestStudio::drawsItself()
{
    focusRow(QStringLiteral("two.md"));
    shot(QStringLiteral("list"));

    key('2');
    shot(QStringLiteral("details-view"));

    key('3');
    shot(QStringLiteral("icons"));

    key('1');
    key('t');
    // The handoff, asserted rather than assumed. `focusTags` returns silently
    // when the panel is not there yet, and the two Enters below were written for
    // a keyboard that is in the tag panel -- with the cursor parked on `two.md`
    // they went to the list instead and opened the file twice. That is the whole
    // of the bug: the picture was of the wrong thing and the suite said nothing.
    QCOMPARE(m_engine->rootObjects().first()->property("focusRegion").toString(),
             QStringLiteral("tags"));
    key(Qt::Key_Return);
    shot(QStringLiteral("tag-filter"));
    key(Qt::Key_Return);
    key(Qt::Key_Escape);

    key('/');
    shot(QStringLiteral("filtering"));
    key(Qt::Key_Escape);

    key('?');
    shot(QStringLiteral("keys"));
    key(Qt::Key_Escape);

    key(':');
    shot(QStringLiteral("commands"));
    key(Qt::Key_Escape);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    shot(QStringLiteral("graph"));
    m_workspace->closeCurrentTab();
    settle();

    QMetaObject::invokeMethod(m_workspace, "splitRight", Q_ARG(QString, QStringLiteral("files")));
    settle();
    shot(QStringLiteral("split"));
    QMetaObject::invokeMethod(m_workspace, "closePane", Q_ARG(int, 1));
    settle();

    QVERIFY2(window()->activeFocusItem() != nullptr,
             "the keyboard was lost somewhere in the tour");
}

// The pictures in `docs/`, taken from a folder built for the purpose.
//
// The fixture the rest of this file uses is named for what each test needs to
// say -- `alpha`, `one.txt`, `plain.txt` -- which is right for a test and wrong
// for a picture: a screenshot of `two.md` in `/tmp/tst_studio-AwEofN` teaches
// nobody what the program is for. This one is a folder somebody might have,
// with sizes and dates far enough apart that the columns have something to
// show.
//
// Skipped unless asked for. Docs are rebuilt on purpose, not as a side effect
// of running the tests, and this one rewrites the index the other tests read --
// which is why it runs last.
void TestStudio::writesTheDocumentationShots()
{
    const QString into = QString::fromLocal8Bit(qgetenv("OMAFILES_SHOTS"));
    if (qgetenv("OMAFILES_DOCS").isEmpty() || into.isEmpty())
        QSKIP("set OMAFILES_DOCS=1 and OMAFILES_SHOTS=<dir> to write the docs images");

    // Fixed dates and a fixed path, so regenerating the images twice gives the
    // same bytes twice: the path is in the picture, on the address bar, so a
    // unique directory per run would put a different string in every image.
    //
    // `/tmp` spelled out rather than `QDir::tempPath()`, which honours `TMPDIR`.
    // On a machine that sets it, the address bar rendered a different string and
    // the byte-compare in `shots-check.sh` failed -- so `git commit` was refused
    // for a reason that had nothing to do with the commit.
    //
    // Fixed and shared is a collision waiting to happen -- two runs at once
    // wrote each other's fixture and both still passed -- so the name is held
    // under a lock for the length of the run. Same discipline as the index: the
    // lock is older than the read, and a second run waits rather than
    // interleaving.
    const QString fixture = QStringLiteral("/tmp/omafiles-docs");
    QVERIFY2(QDir().mkpath(fixture),
             "the documentation pictures need a writable /tmp; the path is in the picture");
    QLockFile held(fixture + QStringLiteral(".lock"));
    // Long enough for a second checkout on the same machine to finish rather
    // than for this one to fail: the fixture path has to be fixed, because it
    // shows in the address bar of every picture, so two runs cannot each have
    // their own and the only options are to queue or to give up.
    QVERIFY2(held.tryLock(120000),
             "another run has held the documentation fixture for two minutes");

    const QString root = fixture + QStringLiteral("/workshop");
    QDir(fixture).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/contracts/2026"));
    QDir().mkpath(root + QStringLiteral("/invoices"));
    QDir().mkpath(root + QStringLiteral("/scans"));

    // A folder made a moment ago is dated a moment ago, so without this the
    // three folder rows carry the clock and the picture differs on every run.
    const auto dateIt = [](const QString &path, const char *when) {
        const QDateTime at = QDateTime::fromString(QString::fromLatin1(when),
                                                   QStringLiteral("yyyy-MM-dd HH:mm"));
        QVERIFY(at.isValid());
        const timespec times[2] = {
            {at.toSecsSinceEpoch(), 0},          // accessed
            {at.toSecsSinceEpoch(), 0},          // modified
        };
        QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0), 0);
    };
    dateIt(root + QStringLiteral("/contracts"), "2026-08-30 20:17");
    dateIt(root + QStringLiteral("/invoices"), "2026-07-21 15:48");
    dateIt(root + QStringLiteral("/scans"), "2026-08-28 07:44");

    struct Sample { const char *file; int kb; const char *when; const char *tags; };
    const Sample samples[] = {
        {"archive-2026.zip",     26, "2026-08-26 20:19", "\"archive\",\"backup\""},
        {"budget.ods",           13, "2026-02-02 15:33", "\"finance\",\"spreadsheet\""},
        {"invoice-4471.pdf",      8, "2026-08-07 12:42", "\"finance\",\"invoice\""},
        {"keyring-fix.md",        7, "2026-08-24 17:29", "\"linux\",\"notes\""},
        {"network.drawio",       21, "2026-02-09 18:22", "\"diagram\",\"home\",\"network\""},
        {"q3-report.ods",        14, "2026-02-19 16:01", "\"finance\",\"q3\",\"spreadsheet\""},
        {"restore-nas.md",       24, "2026-08-14 18:01", "\"backup\",\"notes\""},
        {"scratch.txt",           1, "2026-08-30 09:04", ""},
    };

    QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
    QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));

    for (const Sample &sample : samples) {
        const QString path = QDir(root).filePath(QString::fromLatin1(sample.file));
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(sample.kb * 1024, 'x'));
        }

        // Dated after the write, and on a second handle: the flush at close
        // would put the clock back to now and the modified column would be one
        // date repeated eight times.
        const QDateTime when = QDateTime::fromString(QString::fromLatin1(sample.when),
                                                     QStringLiteral("yyyy-MM-dd HH:mm"));
        QVERIFY(when.isValid());
        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
        }

        if (*sample.tags == '\0')
            continue;                            // one row with no tags, on purpose
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[%2]}\n")
                        .arg(path, QString::fromLatin1(sample.tags))
                        .toUtf8());
    }
    index.close();

    m_index->reload();
    tab()->setCwd(root);
    tab()->setSortRole(QStringLiteral("name"));
    tab()->setSortAscending(true);
    settle();
    focusRow(QStringLiteral("network.drawio"));

    shot(QStringLiteral("studio"), Shot::Documentation);

    key('2');
    shot(QStringLiteral("details"), Shot::Documentation);
    key('1');

    key('?');
    shot(QStringLiteral("keys"), Shot::Documentation);
    key(Qt::Key_Escape);

    m_workspace->addTab(QStringLiteral("graph"));
    settle();
    shot(QStringLiteral("graph"), Shot::Documentation);
    m_workspace->closeCurrentTab();
    settle();
}

// A colour scheme is replaced, not edited: a theme switcher and an editor alike
// write a temporary and rename it over the file. The watch goes with the file it
// was taken on, so a window that does not re-take it follows the first change
// and no later one -- and the desktop and the window disagree about the theme
// until a restart.
void TestStudio::followsEveryThemeChangeAndNotJustTheFirst()
{
    QTemporaryDir state;
    QVERIFY(state.isValid());
    const QString themeDir =
        QDir(state.path()).filePath(QStringLiteral("omarchy/current/theme"));
    QVERIFY(QDir().mkpath(themeDir));
    const QString colours = QDir(themeDir).filePath(QStringLiteral("colors.toml"));

    const auto paint = [&colours](const QString &background) {
        const QString scratch = colours + QStringLiteral(".new");
        {
            QFile file(scratch);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return false;
            const QByteArray body =
                QStringLiteral("background = \"%1\"\n").arg(background).toUtf8();
            if (file.write(body) != body.size())
                return false;
        }
        // `rename(2)` and not QFile::rename, which refuses a destination that is
        // already there -- and a destination that is already there is the whole
        // of what this is about.
        return ::rename(QFile::encodeName(scratch).constData(),
                        QFile::encodeName(colours).constData()) == 0;
    };

    QVERIFY(paint(QStringLiteral("#101010")));

    const QByteArray previous = qgetenv("XDG_STATE_HOME");
    qputenv("XDG_STATE_HOME", state.path().toUtf8());
    const auto restore = qScopeGuard([&previous] { qputenv("XDG_STATE_HOME", previous); });

    Theme theme;
    QCOMPARE(theme.background(), QColor(QStringLiteral("#101010")));

    QVERIFY(paint(QStringLiteral("#202020")));
    QTRY_COMPARE(theme.background(), QColor(QStringLiteral("#202020")));

    // The one that fails without the re-arm: by now the file the watch was taken
    // on has been unlinked once already.
    QVERIFY(paint(QStringLiteral("#303030")));
    QTRY_COMPARE(theme.background(), QColor(QStringLiteral("#303030")));

    QVERIFY(paint(QStringLiteral("#404040")));
    QTRY_COMPARE(theme.background(), QColor(QStringLiteral("#404040")));
}

// Two tabs in one pane are drawn by one model. Marks kept on the model would
// belong to the pane: mark three rows, switch tab, and they are still counted,
// still shown and still what a destructive key would act on -- against a folder
// you cannot see. They belong to the tab, the way the filter and the view mode
// do.
void TestStudio::keepsMarksInTheTabTheyWereMadeIn()
{
    PaneState *pane = m_workspace->paneAtIndex(0);
    QVERIFY(pane);
    const auto folder = [this] {
        return filesRoot()->property("folder").value<QObject *>();
    };
    const auto markedRow = [this](const QString &name) {
        focusRow(name);
        return filesRoot()->property("entry").toMap()
            .value(QStringLiteral("marked")).toBool();
    };

    focusRow(QStringLiteral("one.txt"));
    key(Qt::Key_Space);
    focusRow(QStringLiteral("two.md"));
    key(Qt::Key_Space);
    QCOMPARE(folder()->property("markedCount").toInt(), 2);

    // A second tab in the same pane, standing in the same folder -- so nothing
    // about the directory changes and only the tab does.
    m_workspace->addTab(QStringLiteral("files"));
    settle();
    QCOMPARE(pane->tabCount(), 2);
    QCOMPARE(cwd(), m_tree.path());
    QVERIFY2(folder()->property("markedCount").toInt() == 0,
             "a new tab opened holding the marks made in the tab beside it");

    focusRow(QStringLiteral("three.ods"));
    key(Qt::Key_Space);
    QCOMPARE(folder()->property("markedCount").toInt(), 1);
    QVERIFY(markedRow(QStringLiteral("three.ods")));
    QVERIFY(!markedRow(QStringLiteral("one.txt")));

    // And back, to the two rows this tab marked and no others.
    pane->setCurrentIndex(0);
    settle();
    QCOMPARE(folder()->property("markedCount").toInt(), 2);
    QVERIFY(markedRow(QStringLiteral("one.txt")));
    QVERIFY(markedRow(QStringLiteral("two.md")));
    QVERIFY(!markedRow(QStringLiteral("three.ods")));

    // Walking this tab into another folder ends them: they name files that are
    // no longer on screen.
    tab()->setCwd(m_tree.filePath(QStringLiteral("beta")));
    settle();
    QCOMPARE(folder()->property("markedCount").toInt(), 0);

    while (pane->tabCount() > 1)
        m_workspace->closeTab(0, pane->tabCount() - 1);
    tab()->setCwd(m_tree.path());
    settle();
}

// The list is the folder, so it follows the folder. Anything else writing here
// -- another terminal, a download, `omafiles` itself in the next window -- shows
// up without a key being pressed, and `F5` goes on meaning what the key sheet
// says it means.
void TestStudio::followsTheFolderOnDisk()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QDir here(scratch.path());
    const auto write = [&here](const QString &name) {
        QFile file(here.filePath(name));
        return file.open(QIODevice::WriteOnly) && file.write("x") == 1;
    };
    QVERIFY(write(QStringLiteral("first.txt")));

    tab()->setCwd(scratch.path());
    settle();
    QObject *folder = filesRoot()->property("folder").value<QObject *>();
    QCOMPARE(folder->property("count").toInt(), 2);          // `..` and one file

    QVERIFY(write(QStringLiteral("second.txt")));
    QTRY_COMPARE(folder->property("count").toInt(), 3);
    QVERIFY2(rowOf(QStringLiteral("second.txt")) >= 0,
             "the folder grew a file and the list did not");

    QVERIFY(QFile::remove(here.filePath(QStringLiteral("first.txt"))));
    QTRY_COMPARE(folder->property("count").toInt(), 2);
    QCOMPARE(rowOf(QStringLiteral("first.txt")), -1);

    // A folder is watched wherever the tab walks, not only the one it started
    // in, so the watch has to be re-taken on every read.
    const QString deeper = here.filePath(QStringLiteral("deeper"));
    QVERIFY(QDir().mkpath(deeper));
    tab()->setCwd(deeper);
    settle();
    QCOMPARE(filesRoot()->property("folder").value<QObject *>()
                 ->property("count").toInt(), 1);            // `..` and nothing else
    {
        QFile file(QDir(deeper).filePath(QStringLiteral("later.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QTRY_COMPARE(filesRoot()->property("folder").value<QObject *>()
                     ->property("count").toInt(), 2);

    tab()->setCwd(m_tree.path());
    settle();
}

// The number beside a folder is every name it holds. A count taken from a
// listing that filters on what a stat says each entry is calls a folder of
// dangling links, links round to themselves and fifos empty -- the same failure
// the listing itself was fixed for, in the one place that still read a directory
// its own way.
void TestStudio::countsEveryNameAFolderHolds()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const QString odd = QDir(scratch.path()).filePath(QStringLiteral("odd"));
    QVERIFY(QDir().mkpath(odd));
    const QDir inside(odd);
    // A folder nobody may open, made alongside the other one so both are there
    // before the list is drawn.
    const QString shut = QDir(scratch.path()).filePath(QStringLiteral("shut"));
    QVERIFY(QDir().mkpath(shut));
    QVERIFY(QFile::setPermissions(shut, QFileDevice::Permissions()));
    const auto reopen = qScopeGuard([&shut] {
        QFile::setPermissions(shut, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
    });

    QVERIFY(QFile::link(QStringLiteral("/nowhere/at/all"),
                        inside.filePath(QStringLiteral("dangling"))));
    QVERIFY(QFile::link(QStringLiteral("loop"), inside.filePath(QStringLiteral("loop"))));
    QCOMPARE(::mkfifo(QFile::encodeName(inside.filePath(QStringLiteral("pipe"))).constData(),
                      0644), 0);
    {
        // A dotfile counts: the number is what the folder holds, not what one
        // view of it would draw.
        QFile file(inside.filePath(QStringLiteral(".hidden")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    // And the name that is bytes rather than text. It cannot be listed as a row,
    // and it is still one of the things in there.
    const QByteArray unnameable = QFile::encodeName(odd) + "/bad\xffname";
    const int fd = ::open(unnameable.constData(), O_CREAT | O_WRONLY, 0644);
    QVERIFY(fd >= 0);
    ::close(fd);
    const auto sweep = qScopeGuard([&unnameable] { ::unlink(unnameable.constData()); });

    tab()->setCwd(scratch.path());
    settle();
    focusRow(QStringLiteral("odd"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("items")).toInt(), 5);

    // A folder nobody may open is not a folder with nothing in it, and the panel
    // says so by not putting a number there at all.
    focusRow(QStringLiteral("shut"));
    QCOMPARE(filesRoot()->property("entry").toMap()
                 .value(QStringLiteral("items")).toInt(), -1);

    tab()->setCwd(m_tree.path());
    settle();
}


void TestStudio::pickSortRoleTogglesOnSameRole()
{
    tab()->pickSortRole(QStringLiteral("name"));
    const bool before = tab()->sortAscending();
    tab()->pickSortRole(QStringLiteral("name"));
    QVERIFY(before != tab()->sortAscending());
}


// Dolphin and Nautilus both end a clipped tag row with +N rather than silently
// dropping names. The strip exposes how many are hidden so a test can prove the
// count matches what opacity hid, with space reserved for the chip itself.
void TestStudio::tagStripSaysHowManyTagsDoNotFit()
{
    const QString path = QDir(m_tree.path()).filePath(QStringLiteral("manytags.txt"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    QStringList tags;
    QStringList jsonTags;
    for (int i = 0; i < 16; ++i) {
        const QString tag = QString(QChar('a' + i));
        tags << tag;
        jsonTags << QStringLiteral("\"%1\"").arg(tag);
    }

    {
        QFile index(QDir(m_home.path()).filePath(QStringLiteral("omafiles/index.jsonl")));
        QVERIFY(index.open(QIODevice::Append | QIODevice::Text));
        index.write(QStringLiteral("{\"path\":\"%1\",\"tags\":[%2]}\n")
                        .arg(path, jsonTags.join(QLatin1Char(',')))
                        .toUtf8());
    }
    m_index->reload();
    settle();

    tab()->setViewMode(QStringLiteral("details"));
    settle();
    focusRow(QStringLiteral("manytags.txt"));
    settle();

    QObject *overflowing = nullptr;
    for (QQuickItem *item : itemsNamed(QStringLiteral("tagStrip"))) {
        if (item->property("tags").toStringList().size() != tags.size())
            continue;
        if (!item->isVisible())
            continue;
        if (item->property("hiddenCount").toInt() <= 0)
            continue;
        overflowing = item;
        break;
    }
    QVERIFY2(overflowing, "no tag strip reported hidden tags for manytags.txt");
    QCOMPARE(overflowing->property("tags").toStringList(), tags);
    QVERIFY(overflowing->property("hiddenCount").toInt() > 0);
    QCOMPARE(overflowing->property("hiddenCount").toInt()
                 + overflowing->property("visibleCount").toInt(),
             tags.size());

    auto *strip = qobject_cast<QQuickItem *>(overflowing);
    QVERIFY(strip);
    const bool alignRight = strip->property("alignRight").toBool();
    const QString remainderName = alignRight
            ? QStringLiteral("tagStripOverflowLeading")
            : QStringLiteral("tagStripOverflowTrailing");
    QQuickItem *remainder = nullptr;
    QList<QQuickItem *> queue{strip};
    while (!queue.isEmpty()) {
        QQuickItem *item = queue.takeFirst();
        if (item->objectName() == remainderName) {
            remainder = item;
            break;
        }
        queue.append(item->childItems());
    }
    QVERIFY2(remainder, "no +N remainder chip on overflowing tag strip");
    QVERIFY(remainder->isVisible());
    const QPointF topLeft = remainder->mapToItem(strip, QPointF(0, 0));
    QVERIFY2(topLeft.x() >= 0.0,
             qPrintable(QStringLiteral("+N chip x=%1 is left of strip").arg(topLeft.x())));
    QVERIFY2(topLeft.x() + remainder->width() <= strip->width() + 0.5,
             qPrintable(QStringLiteral("+N chip right edge %1 exceeds strip width %2")
                                .arg(topLeft.x() + remainder->width())
                                .arg(strip->width())));

    focusRow(QStringLiteral("two.md"));
    settle();

    QObject *fits = nullptr;
    for (QQuickItem *item : itemsNamed(QStringLiteral("tagStrip"))) {
        if (item->property("tags").toStringList().size() != 1)
            continue;
        fits = item;
        break;
    }
    QVERIFY2(fits, "no tag strip found for two.md");
    QCOMPARE(fits->property("hiddenCount").toInt(), 0);
    QCOMPARE(fits->property("visibleCount").toInt(), 1);
}

QTEST_MAIN(TestStudio)
#include "tst_studio.moc"
