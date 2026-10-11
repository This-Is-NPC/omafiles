#include <QtTest>

#include "Paths.h"
#include "Recent.h"
#include "Record.h"
#include "Roots.h"
#include "Store.h"
#include "Tag.h"
#include "View.h"
#include "Xattr.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>

// A `readdir` that can be made to fail, for the one branch no filesystem will
// produce on demand: a directory read that dies half way through.
//
// A definition here stands in front of the C library's for the whole process --
// the executable is what the dynamic linker resolves first -- so with the
// counter at zero every call is handed straight on, and it disarms itself on the
// call it fails.
static int g_readdirFailsOnCall = 0;

extern "C" struct dirent *readdir(DIR *stream)
{
    static auto passThrough =
        reinterpret_cast<struct dirent *(*)(DIR *)>(::dlsym(RTLD_NEXT, "readdir"));
    if (g_readdirFailsOnCall > 0 && --g_readdirFailsOnCall == 0) {
        errno = EIO;
        return nullptr;
    }
    return passThrough(stream);
}

class StoreTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_home;

private slots:
    // Every directory the core finds files through, so no test reads or writes
    // the real index, roots, cache or state of whoever runs it.
    void initTestCase()
    {
        QVERIFY(m_home.isValid());
        const QDir home(m_home.path());
        for (const char *variable : {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                     "XDG_STATE_HOME", "HOME"})
            qputenv(variable, QFile::encodeName(home.filePath(QLatin1String(variable))));
    }

    void rejectsInvalidTags()
    {
        QString error;
        QVERIFY(omafiles::normalizeTag(QStringLiteral("work"), &error) == QStringLiteral("work"));
        QVERIFY(omafiles::normalizeTag(QStringLiteral("Work"), &error) == QStringLiteral("work"));
        QVERIFY(omafiles::normalizeTag(QStringLiteral("-nope"), &error).isEmpty());
        QVERIFY(omafiles::normalizeTag(QStringLiteral("has space"), &error).isEmpty());
        QVERIFY(omafiles::normalizeTag(QString(65, QLatin1Char('a')), &error).isEmpty());
        QCOMPARE(omafiles::normalizeTag(QString(64, QLatin1Char('a')), &error).size(), 64);
    }

    void writesAndReadsJsonl()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(QStringLiteral("index.jsonl"));
        omafiles::Store store(index);
        // A writer takes the lock before it reads: `load` is the read-only
        // door, and `commit` refuses through it.
        QVERIFY(store.lockAndLoad(nullptr));

        omafiles::Record record;
        record.path = QStringLiteral("/tmp/invoice.pdf");
        record.tags = QStringList{QStringLiteral("work"), QStringLiteral("finance")};
        store.put(record);
        QString error;
        QVERIFY(store.commit(&error));

        omafiles::Store again(index);
        QVERIFY(again.load(&error));
        QCOMPARE(again.get(record.path).tags, QStringList() << QStringLiteral("finance")
                                                            << QStringLiteral("work"));
        QVERIFY(store.lockAndLoad(&error));
        record.tags.clear();
        store.put(record);
        QVERIFY(store.commit(&error));
        QVERIFY(again.load(&error));
        QVERIFY(!again.contains(record.path));
    }

    // Absent has to mean "unknown" and survive a round trip as absent, or an
    // undated row comes back claiming it was tagged at the epoch.
    void carriesWhenTagsChanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(QStringLiteral("index.jsonl"));

        omafiles::Record stamped;
        stamped.path = QStringLiteral("/tmp/stamped.md");
        stamped.tags = QStringList{QStringLiteral("notes")};
        stamped.tagged = 1788300000;

        omafiles::Record old;
        old.path = QStringLiteral("/tmp/old.md");
        old.tags = QStringList{QStringLiteral("notes")};

        QVERIFY(!stamped.toJson().value(QStringLiteral("tagged")).isUndefined());
        QVERIFY(old.toJson().value(QStringLiteral("tagged")).isUndefined());

        omafiles::Store store(index);
        QVERIFY(store.lockAndLoad(nullptr));
        store.put(stamped);
        store.put(old);
        QString error;
        QVERIFY(store.commit(&error));

        omafiles::Store again(index);
        QVERIFY(again.load(&error));
        QCOMPARE(again.get(stamped.path).tagged, 1788300000);
        QCOMPARE(again.get(old.path).tagged, 0);
    }

    // The lock is the whole read-modify-write, not the write: locking only the
    // write makes every concurrent tagging run a lost update, each process
    // adding its row to its own copy and the last to commit winning.
    void refusesToWriteWithoutTheLock()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(QStringLiteral("index.jsonl"));

        omafiles::Store reader(index);
        QVERIFY(reader.load(nullptr));
        QVERIFY(!reader.holdsLock());

        omafiles::Record record;
        record.path = QStringLiteral("/tmp/unlocked.md");
        record.tags = QStringList{QStringLiteral("notes")};
        reader.put(record);

        QString error;
        QVERIFY2(!reader.commit(&error), "a read-only load was allowed to write");
        QVERIFY(error.contains(QStringLiteral("without the lock")));
        QVERIFY(!QFile::exists(index));

        // And a second writer cannot take a lock the first is holding. Told no
        // quickly rather than after the five seconds a person at a terminal
        // should be willing to wait.
        omafiles::Store first(index);
        QVERIFY(first.lockAndLoad(&error));
        QVERIFY(first.holdsLock());
        // At the one name `lockPath` gives, which the CLI's file verbs refuse.
        QVERIFY(QFile::exists(omafiles::Store::lockPath(index)));

        omafiles::Store second(index);
        second.setLockTimeout(50);
        QVERIFY2(!second.lockAndLoad(&error), "two writers held the lock at once");
        QVERIFY(!second.holdsLock());

        // The lock goes when the write lands, so the next writer gets in.
        first.put(record);
        QVERIFY(first.commit(&error));
        QVERIFY(!first.holdsLock());
        QVERIFY(second.lockAndLoad(&error));
        QCOMPARE(second.get(record.path).tags, QStringList{QStringLiteral("notes")});
    }

    // A row with no tags is not a row: `put` drops it when the last tag comes
    // off, and `load` refuses a line holding one, which no writer here keeps.
    void keepsNoRowWithoutTags()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(QStringLiteral("index.jsonl"));

        omafiles::Store store(index);
        QString error;
        QVERIFY(store.lockAndLoad(&error));

        omafiles::Record record;
        record.path = QStringLiteral("/tmp/emptied.md");
        record.tags = QStringList{QStringLiteral("notes")};
        store.put(record);
        QVERIFY(store.contains(record.path));

        // Taking the last tag off takes the row with it.
        record.tags.clear();
        store.put(record);
        QVERIFY2(!store.contains(record.path), "a row with no tags stayed in the index");
        QVERIFY(store.commit(&error));

        // And one that reaches the file some other way -- a hand-edited line
        // -- is a line this program never writes, refused rather than dropped.
        {
            QFile handle(index);
            QVERIFY(handle.open(QIODevice::Append | QIODevice::Text));
            handle.write("{\"path\":\"/tmp/tagless.md\",\"tags\":[]}\n");
        }
        omafiles::Store reread(index);
        QVERIFY2(!reread.load(&error), "a tagless row in the file was loaded");
        QVERIFY2(error.contains(QStringLiteral("no tags")), qPrintable(error));
    }

    // Pins the default wait down. The end-to-end races assert only that no
    // writer loses a tag in silence, which stays true if the wait is cut to a
    // millisecond and most of them give up loudly instead; a writer arriving
    // while another is mid-write has to wait for it, not walk away.
    void waitsForTheWriterAlreadyIn()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(QStringLiteral("index.jsonl"));

        omafiles::Record record;
        record.path = QStringLiteral("/tmp/waited.md");
        record.tags = QStringList{QStringLiteral("notes")};

        omafiles::Store holder(index);
        QString held;
        QVERIFY(holder.lockAndLoad(&held));
        holder.put(record);

        // Let go half a second from now: far past a wait of nothing, far
        // inside the five seconds somebody at a terminal will give it. On a
        // thread because `lockAndLoad` blocks without spinning an event loop,
        // so a timer would never fire.
        bool landed = false;
        std::thread releaser([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            QString error;
            landed = holder.commit(&error);
        });

        omafiles::Store waiting(index);          // the default wait, deliberately
        QElapsedTimer clock;
        clock.start();
        QString error;
        const bool got = waiting.lockAndLoad(&error);
        const qint64 waited = clock.elapsed();
        releaser.join();

        QVERIFY2(landed, "the first writer never committed");
        QVERIFY2(got, qPrintable(QStringLiteral("gave up after %1 ms on a lock released "
                                                "at 500 ms: %2").arg(waited).arg(error)));
        QVERIFY2(waited >= 400, "it did not wait for the lock at all");
        QCOMPARE(waiting.get(record.path).tags, QStringList{QStringLiteral("notes")});
    }

    // A lock file that names no pid is foreign only where it is the file that
    // stood there all the wait: one put in its place is a writer's, mid-write.
    void namesAForeignLockOnlyWhereItStayed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString index = dir.filePath(QStringLiteral("index.jsonl"));
        const QString lockFile = omafiles::Store::lockPath(index);
        QFile foreign(lockFile);
        QVERIFY(foreign.open(QIODevice::WriteOnly));
        foreign.close();

        omafiles::Store stayed(index);
        stayed.setLockTimeout(200);
        QString error;
        QVERIFY(!stayed.lockAndLoad(&error));
        QVERIFY(!stayed.contended());
        QVERIFY2(error.contains(QStringLiteral("no omafiles wrote")), qPrintable(error));

        // Replaced by rename, so no moment passes with the lock free.
        std::thread writer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const QString fresh = dir.filePath(QStringLiteral("fresh"));
            QFile file(fresh);
            if (file.open(QIODevice::WriteOnly))
                file.close();
            ::rename(QFile::encodeName(fresh).constData(), QFile::encodeName(lockFile).constData());
        });
        omafiles::Store replaced(index);
        replaced.setLockTimeout(2000);
        const bool got = replaced.lockAndLoad(&error);
        writer.join();
        QVERIFY(!got);
        QVERIFY(replaced.contended());
        QCOMPARE(error,
                 QStringLiteral("store: cannot lock %1: another writer holds it").arg(index));
    }

    // `readdir` returns null for the end of a directory and for a failure alike,
    // and only errno tells them apart. A read that stopped part way is not a
    // complete listing, and answering with one is how a caller comes to act on a
    // directory it only saw the beginning of.
    void saysWhenADirectoryReadStopsPartWay()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (const char *name : {"one", "two", "three"}) {
            QFile file(QDir(dir.path()).filePath(QString::fromLatin1(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }

        QStringList names;
        QString error;
        omafiles::PathState state = omafiles::PathState::Unknown;
        QVERIFY(omafiles::readDirectory(dir.path(), &names, nullptr, &error, &state));
        QCOMPARE(names.size(), 3);
        QCOMPARE(state, omafiles::PathState::Present);

        names.clear();
        error.clear();
        g_readdirFailsOnCall = 2;
        const bool whole = omafiles::readDirectory(dir.path(), &names, nullptr, &error, &state);
        g_readdirFailsOnCall = 0;

        QVERIFY2(!whole, "a directory read that stopped part way came back as a whole listing");
        QVERIFY2(error.contains(QString::fromLocal8Bit(strerror(EIO))),
                 qPrintable(QStringLiteral("the failure was not named: %1").arg(error)));
        // The system declined to finish, which is not the same answer as the
        // directory not being there.
        QCOMPARE(state, omafiles::PathState::Unknown);
    }

    // The store side of a declared root: a row written as an id and the rest of
    // the way there, resolved back through whatever the declaration says today.
    void storesARowRelativeToItsRoot()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir tree(home.path());
        QVERIFY(tree.mkpath(QStringLiteral("here/docs")));
        QVERIFY(tree.mkpath(QStringLiteral("there/docs")));
        const QString first = tree.filePath(QStringLiteral("here"));
        const QString second = tree.filePath(QStringLiteral("there"));
        QFile file(tree.filePath(QStringLiteral("here/docs/a.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("a");
        file.close();

        const auto put = [](omafiles::Store *store, const QString &path) {
            omafiles::Record row;
            row.path = path;
            row.tags = {QStringLiteral("work")};
            store->put(row);
        };

        const QString index = tree.filePath(QStringLiteral("index.jsonl"));
        const QVector<omafiles::Root> before =
            omafiles::setRoots({omafiles::Root{QStringLiteral("archive"), first, false, {}}});

        QString error;
        {
            omafiles::Store store(index);
            QVERIFY2(store.lockAndLoad(&error), qPrintable(error));
            put(&store, tree.filePath(QStringLiteral("here/docs/a.txt")));
            QVERIFY2(store.commit(&error), qPrintable(error));
        }

        // Written as the two halves, not as the absolute path: that is what
        // survives the storage arriving somewhere else.
        QFile written(index);
        QVERIFY(written.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString line = QString::fromUtf8(written.readAll()).trimmed();
        written.close();
        QVERIFY2(line.contains(QStringLiteral("\"root\":\"archive\"")), qPrintable(line));
        QVERIFY2(line.contains(QStringLiteral("\"path\":\"docs/a.txt\"")), qPrintable(line));
        QVERIFY2(!line.contains(first), qPrintable(line));

        // Declared somewhere else, and the row is that file over there. Nothing
        // was rewritten to make that true.
        QVERIFY(QFile::rename(tree.filePath(QStringLiteral("here/docs/a.txt")),
                              tree.filePath(QStringLiteral("there/docs/a.txt"))));
        omafiles::setRoots({omafiles::Root{QStringLiteral("archive"), second, false, {}}});
        {
            omafiles::Store store(index);
            QVERIFY2(store.load(&error), qPrintable(error));
            QVERIFY(store.contains(tree.filePath(QStringLiteral("there/docs/a.txt"))));
            QCOMPARE(store.get(tree.filePath(QStringLiteral("there/docs/a.txt"))).tags,
                     QStringList{QStringLiteral("work")});
        }

        // A row naming a declaration that is not there is refused by name, not
        // guessed at: putting the line back is a repair that loses nothing,
        // and placing the row anywhere else would lose the tags.
        omafiles::setRoots({});
        {
            omafiles::Store store(index);
            QVERIFY2(!store.load(&error), "a row under an undeclared root loaded anyway");
            QVERIFY2(error.contains(QStringLiteral("archive")), qPrintable(error));
        }

        omafiles::setRoots(before);
    }

    // An unmounted tree is `ENOENT` from a stat, which is what every file in it
    // having been deleted looks like -- and `prune` acts on that answer.
    void tellsAnUnpluggedRootFromADeletedFile()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir tree(home.path());
        QVERIFY(tree.mkpath(QStringLiteral("mount/docs")));
        const QString mount = tree.filePath(QStringLiteral("mount"));
        const QString marker = tree.filePath(QStringLiteral("mount/.plugged-in"));
        QFile stamp(marker);
        QVERIFY(stamp.open(QIODevice::WriteOnly));
        stamp.close();

        const QString absent = tree.filePath(QStringLiteral("mount/docs/gone.txt"));
        const QVector<omafiles::Root> before = omafiles::setRoots({});

        // Undeclared, it reads as deleted, which is today's answer and stays
        // the answer for every machine that declares nothing.
        QString error;
        QCOMPARE(omafiles::pathState(absent, &error), omafiles::PathState::Missing);

        omafiles::setRoots({omafiles::Root{QStringLiteral("archive"), mount, false,
                                           QStringLiteral(".plugged-in")}});
        // Declared and plugged in, it still reads as deleted: the storage is
        // here and the file really is not.
        QCOMPARE(omafiles::pathState(absent, &error), omafiles::PathState::Missing);

        // Unplugged, and the same syscall means something else entirely.
        QVERIFY(QFile::remove(marker));
        QCOMPARE(omafiles::pathState(absent, &error), omafiles::PathState::Unreachable);
        QVERIFY2(error.contains(QStringLiteral("archive")), qPrintable(error));

        omafiles::setRoots(before);
    }

    // Only ENOENT is absence. A file under a directory nobody may search is
    // there for all anybody knows, and reading it as empty is an index, a set
    // of roots or a set of views silently answered out of nothing.
    void readsAShutFileAsAFailureNotAsEmpty()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir tree(home.path());
        QVERIFY(tree.mkpath(QStringLiteral("shut")));
        const QString shut = tree.filePath(QStringLiteral("shut"));
        for (const char *name : {"index.jsonl", "roots.jsonl", "views.jsonl"}) {
            QFile file(QDir(shut).filePath(QLatin1String(name)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
        }
        QVERIFY(QFile::setPermissions(shut, {}));
        QString error;
        omafiles::Store store(QDir(shut).filePath(QStringLiteral("index.jsonl")));
        const bool storeLoaded = store.load(&error);
        const QString storeError = error;
        const QVector<omafiles::Root> before = omafiles::roots();
        const bool rootsLoaded
            = omafiles::loadRoots(QDir(shut).filePath(QStringLiteral("roots.jsonl")), &error);
        const QString rootsError = error;
        omafiles::setRoots(before);
        QVector<omafiles::View> views;
        const bool viewsLoaded
            = omafiles::loadViews(QDir(shut).filePath(QStringLiteral("views.jsonl")), &views, &error);
        QVERIFY(QFile::setPermissions(shut, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

        QVERIFY2(!storeLoaded, "an index nobody may read loaded as empty");
        QVERIFY2(storeError.contains(QString::fromLocal8Bit(strerror(EACCES))), qPrintable(storeError));
        QVERIFY2(!rootsLoaded, "a roots file nobody may read loaded as none declared");
        QVERIFY2(rootsError.contains(QString::fromLocal8Bit(strerror(EACCES))), qPrintable(rootsError));
        QVERIFY2(!viewsLoaded, "a views file nobody may read loaded as no views");
        QVERIFY2(error.contains(QString::fromLocal8Bit(strerror(EACCES))), qPrintable(error));

        // The desktop's list is found through the environment, not handed a
        // path, so the environment is pointed into the shut directory.
        const QByteArray dataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", QFile::encodeName(shut));
        QVERIFY(QFile::setPermissions(shut, {}));
        QVector<omafiles::RecentEntry> opened;
        const bool recentRead = omafiles::recentlyUsed(&opened, nullptr, &error);
        QVERIFY(QFile::setPermissions(shut, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("XDG_DATA_HOME", dataHome);
        QVERIFY2(!recentRead, "a desktop list nobody may read read as nothing opened");
        QVERIFY2(error.contains(QString::fromLocal8Bit(strerror(EACCES))), qPrintable(error));

        // A directory where the file should be opens and reads as nothing.
        QVERIFY(tree.mkpath(QStringLiteral("folder.jsonl")));
        omafiles::Store folder(tree.filePath(QStringLiteral("folder.jsonl")));
        QVERIFY2(!folder.load(&error), "a directory loaded as an empty index");
        QVERIFY2(error.contains(QString::fromLocal8Bit(strerror(EISDIR))), qPrintable(error));

        // And a file that is not there still is none.
        omafiles::Store fresh(tree.filePath(QStringLiteral("nothing/index.jsonl")));
        QVERIFY(fresh.load(&error));
    }

    // ENOTDIR is the system stopping at a component that is a file, which is
    // an answer about that component and not "no such name".
    void readsNotADirectoryAsTheSystemsAnswer()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString file = QDir(home.path()).filePath(QStringLiteral("plain"));
        QFile touch(file);
        QVERIFY(touch.open(QIODevice::WriteOnly));
        touch.close();
        QString error;
        QCOMPARE(omafiles::pathState(file + QStringLiteral("/inside"), &error),
                 omafiles::PathState::Unknown);
        QVERIFY2(error.contains(QString::fromLocal8Bit(strerror(ENOTDIR))), qPrintable(error));
    }

    void spellsOnlyTheBytesThatSurviveAsText()
    {
        QString name;
        QVERIFY(omafiles::nameFromBytes(QByteArray("caf\xc3\xa9"), &name));
        QCOMPARE(name, QString::fromUtf8("caf\xc3\xa9"));
        QVERIFY(!omafiles::nameFromBytes(QByteArray("\xff\xfe"), &name));
        QVERIFY(!omafiles::nameFromBytes(QByteArray("a\0b", 3), &name));
        QCOMPARE(omafiles::escapedBytes(QByteArray("a\xff\\\xc3\xa9")),
                 QString::fromUtf8("a\\xff\\\\\xc3\xa9"));
    }

    // Each hop, so the one to repoint is on the page.
    void namesTheEndOfADanglingChain()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir tree(home.path());
        QVERIFY(QFile::link(tree.filePath(QStringLiteral("lb")), tree.filePath(QStringLiteral("la"))));
        QVERIFY(QFile::link(tree.filePath(QStringLiteral("nowhere")),
                            tree.filePath(QStringLiteral("lb"))));
        QString error;
        QVERIFY(omafiles::canonicalPath(tree.filePath(QStringLiteral("la")), &error).isEmpty());
        QCOMPARE(error, QStringLiteral("%1 is a symlink to %2, which is a symlink to %3, which is "
                                       "not there")
                            .arg(tree.filePath(QStringLiteral("la")),
                                 tree.filePath(QStringLiteral("lb")),
                                 tree.filePath(QStringLiteral("nowhere"))));
    }

    // The declaration is the answer: a root that is away holds nothing that is
    // here, whatever still answers at its path. And a root nobody may check is
    // neither here nor away.
    void aRootsDeclarationWinsOverTheDisk()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir tree(home.path());
        QVERIFY(tree.mkpath(QStringLiteral("mount/docs")));
        const QString mount = tree.filePath(QStringLiteral("mount"));
        const QString stray = tree.filePath(QStringLiteral("mount/docs/stray.txt"));
        QFile file(stray);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        const QVector<omafiles::Root> before = omafiles::setRoots(
            {omafiles::Root{QStringLiteral("archive"), mount, false, QStringLiteral(".plugged-in")},
             omafiles::Root{QStringLiteral("disk"), tree.filePath(QStringLiteral("mount/docs")),
                            true, QString()}});
        QString error;
        QCOMPARE(omafiles::pathState(mount + QStringLiteral("/other"), &error),
                 omafiles::PathState::Unreachable);
        // Not a mount point, so the disk root is away over a file that stats.
        QCOMPARE(omafiles::pathState(stray, &error), omafiles::PathState::Unreachable);
        QVERIFY2(error.contains(QStringLiteral("disk")), qPrintable(error));

        QVERIFY(QFile::setPermissions(mount, {}));
        const omafiles::PathState checked = omafiles::rootState(omafiles::roots().at(0), &error);
        const QString checkedError = error;
        const omafiles::PathState mounted = omafiles::rootState(omafiles::roots().at(1), &error);
        QVERIFY(QFile::setPermissions(mount, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        omafiles::setRoots(before);
        QCOMPARE(checked, omafiles::PathState::Unknown);
        QVERIFY2(checkedError.contains(QString::fromLocal8Bit(strerror(EACCES))),
                 qPrintable(checkedError));
        QCOMPARE(mounted, omafiles::PathState::Unknown);
    }

    // U+FEFF is a character a name may begin with, though Qt's own decoder
    // drops a leading one.
    void keepsAByteOrderMarkAName()
    {
        const QByteArray bytes("\xef\xbb\xbfx");
        QString name;
        QVERIFY(omafiles::nameFromBytes(bytes, &name));
        QCOMPARE(name, QString(QChar(0xfeff)) + QLatin1Char('x'));
        QCOMPARE(omafiles::escapedBytes(bytes), name);
        QCOMPARE(omafiles::withoutBom(bytes), QByteArray("x"));
        QCOMPARE(omafiles::withoutBom(QByteArray("x\xef\xbb\xbf")), QByteArray("x\xef\xbb\xbf"));
    }

    // A row is read back only in the shape this program writes it.
    void readsOnlyTheRowsItWrites()
    {
        const QVector<omafiles::Root> before = omafiles::setRoots(
            {omafiles::Root{QStringLiteral("r"), QStringLiteral("/srv/r"), false, {}}});
        const auto refused = [](const QJsonObject &row) {
            QString error;
            return omafiles::Record::fromJson(row, &error).path.isEmpty() && !error.isEmpty();
        };
        const auto row = [](const QString &path, const QJsonArray &tags) {
            return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("tags"), tags}};
        };
        QVERIFY(!refused(row(QStringLiteral("/a/b"), {QStringLiteral("work")})));
        for (const QString &tag : {QStringLiteral("Bad Tag"), QStringLiteral("UPPER"), QString()})
            QVERIFY2(refused(row(QStringLiteral("/a/b"), {tag})), qPrintable(tag));
        QVERIFY(refused(row(QStringLiteral("/a/b") + QChar(0) + QStringLiteral("c"), {})));
        QVERIFY(refused(row(QStringLiteral("/a/") + QChar(0xd800), {})));
        QVERIFY(refused(row(QStringLiteral("rel/path"), {})));
        QVERIFY(refused(row(QStringLiteral("//a/b/"), {})));
        QVERIFY(refused(row(QStringLiteral("/a/./b"), {})));
        QJsonObject rooted = row(QStringLiteral("docs/a"), {QStringLiteral("work")});
        rooted.insert(QStringLiteral("root"), QStringLiteral("r"));
        QVERIFY(!refused(rooted));
        for (const QString &relative : {QStringLiteral("../out"), QStringLiteral("/srv/r/a"),
                                        QStringLiteral("a/../b"), QStringLiteral("..")}) {
            rooted.insert(QStringLiteral("path"), relative);
            QVERIFY2(refused(rooted), qPrintable(relative));
        }
        omafiles::setRoots(before);
    }

    // A row whose path moved out of its root is written where it is now, not
    // back under the root it was read from.
    void writesAMovedRowWhereItIs()
    {
        const QVector<omafiles::Root> before = omafiles::setRoots(
            {omafiles::Root{QStringLiteral("r"), QStringLiteral("/srv/r"), false, {}}});
        QString error;
        omafiles::Record record = omafiles::Record::fromJson(
            QJsonObject{{QStringLiteral("root"), QStringLiteral("r")},
                        {QStringLiteral("path"), QStringLiteral("d/x")},
                        {QStringLiteral("tags"), QJsonArray{QStringLiteral("t")}}},
            &error);
        QCOMPARE(record.path, QStringLiteral("/srv/r/d/x"));
        record.path = QStringLiteral("/home/outd/x");
        const QJsonObject stored = record.toStoredJson();
        QVERIFY2(!stored.contains(QStringLiteral("root")), qPrintable(error));
        QCOMPARE(stored.value(QStringLiteral("path")).toString(), QStringLiteral("/home/outd/x"));
        // Undeclared since it was read, it keeps the form it was read in.
        omafiles::setRoots({});
        record.path = QStringLiteral("/srv/r/d/x");
        QCOMPARE(record.toStoredJson().value(QStringLiteral("root")).toString(), QStringLiteral("r"));
        omafiles::setRoots(before);
    }

    // A path typed relative, or through `..` after a symlink, finds its root
    // where the kernel resolves it.
    void findsTheRootOfATypedPath()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir tree(home.path());
        QVERIFY(tree.mkpath(QStringLiteral("disk/sub")));
        QVERIFY(tree.mkpath(QStringLiteral("elsewhere/in")));
        QVERIFY(QFile::link(tree.filePath(QStringLiteral("disk/sub")),
                            tree.filePath(QStringLiteral("elsewhere/in/L"))));
        const QString disk = QFileInfo(tree.filePath(QStringLiteral("disk"))).canonicalFilePath();
        const QVector<omafiles::Root> before = omafiles::setRoots(
            {omafiles::Root{QStringLiteral("disk"), disk, false, QStringLiteral(".here")}});
        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(home.path()));
        QString error;
        const omafiles::PathState relative = omafiles::pathState(QStringLiteral("disk/gone"), &error);
        const QString relativeError = error;
        const omafiles::PathState through =
            omafiles::pathState(QStringLiteral("elsewhere/in/L/../gone"), &error);
        omafiles::PathState listed = omafiles::PathState::Present;
        omafiles::readDirectory(QStringLiteral("disk/gone"), nullptr, nullptr, nullptr, &listed);
        QVERIFY(QDir::setCurrent(previous));
        omafiles::setRoots(before);
        QCOMPARE(relative, omafiles::PathState::Unreachable);
        QVERIFY2(relativeError.contains(QStringLiteral("root disk is declared and not here")),
                 qPrintable(relativeError));
        QCOMPARE(through, omafiles::PathState::Unreachable);
        QCOMPARE(listed, omafiles::PathState::Unreachable);
    }

    void splitsAPathAsTyped()
    {
        QString name;
        QCOMPARE(omafiles::typedDirectory(QStringLiteral("L/../c2"), &name), QStringLiteral("L/.."));
        QCOMPARE(name, QStringLiteral("c2"));
        QCOMPARE(omafiles::typedDirectory(QStringLiteral("c2"), &name), QStringLiteral("."));
        QCOMPARE(omafiles::typedDirectory(QStringLiteral("/mm"), &name), QStringLiteral("/"));
        QCOMPARE(name, QStringLiteral("mm"));
        QCOMPARE(omafiles::typedDirectory(QStringLiteral("a/b//"), &name), QStringLiteral("a"));
        QCOMPARE(name, QStringLiteral("b"));
        QVERIFY(omafiles::isCleanRelative(QStringLiteral("a/b")));
        for (const char *path : {"", "/a", "a/", "a//b", "./a", "a/../b", "..", "../a"})
            QVERIFY2(!omafiles::isCleanRelative(QString::fromLatin1(path)), path);
        QCOMPARE(omafiles::shellQuoted(QStringLiteral("/a/b.txt")), QStringLiteral("/a/b.txt"));
        QCOMPARE(omafiles::shellQuoted(QStringLiteral("a b'c")),
                 QStringLiteral("'a b'\"'\"'c'"));
    }

    // A declaration nothing here writes is refused, not placed.
    void refusesADeclarationRootsAddWouldNotWrite()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString file = QDir(home.path()).filePath(QStringLiteral("roots.jsonl"));
        const QVector<omafiles::Root> before = omafiles::setRoots({});
        for (const QString &line :
             {QStringLiteral("{\"id\":\"../../x\",\"path\":\"/srv\"}"),
              QStringLiteral("{\"id\":\"s\",\"path\":\"srv\"}"),
              QStringLiteral("{\"id\":\"s\",\"path\":\"/srv\",\"check\":\"../m\"}"),
              QStringLiteral("{\"id\":\"s\",\"path\":\"/srv\",\"check\":\"/m\"}"),
              QStringLiteral("{\"id\":\"Upper\",\"path\":\"/srv\"}"),
              QStringLiteral("{\"id\":\"s\",\"path\":\"/srv/\"}")}) {
            QFile out(file);
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(line.toUtf8() + '\n');
            out.close();
            QString error;
            QVERIFY2(!omafiles::loadRoots(file, &error), qPrintable(line));
            QVERIFY2(error.contains(file), qPrintable(error));
        }
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write("{\"id\":\"kept\",\"path\":\"/srv\",\"check\":\"m/k\"}\n");
        out.close();
        QString error;
        QVERIFY2(omafiles::loadRoots(file, &error), qPrintable(error));
        QCOMPARE(omafiles::roots().at(0).path, QStringLiteral("/srv"));
        omafiles::setRoots(before);
    }

    // The XDG Base Directory spec: a relative value is invalid and ignored.
    void ignoresARelativeBaseDirectory()
    {
        const QByteArray previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", "rel");
        const QString decided = omafiles::dataHome();
        qputenv("XDG_DATA_HOME", previous);
        QCOMPARE(decided, QDir::homePath() + QStringLiteral("/.local/share"));
    }

    // A current directory that was deleted is refused, not spelled "".
    void refusesACurrentDirectoryThatIsGone()
    {
        const QString previous = QDir::currentPath();
        QString error;
        QVERIFY2(omafiles::workingDirectoryNameable(&error), qPrintable(error));
        {
            QTemporaryDir gone;
            QVERIFY(gone.isValid());
            QVERIFY(QDir::setCurrent(gone.path()));
        }
        const bool nameable = omafiles::workingDirectoryNameable(&error);
        QVERIFY(QDir::setCurrent(previous));
        QVERIFY(!nameable);
        QVERIFY2(error.contains(QString::fromLocal8Bit(strerror(ENOENT))), qPrintable(error));
    }

    // `realpath` to bytes that are not UTF-8 is refused, never spelled with
    // U+FFFD, and says so apart from every other refusal.
    void refusesAResolutionItCannotSpell()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray bad = QFile::encodeName(dir.path()) + "/bad\xff";
        QVERIFY(::close(::open(bad.constData(), O_CREAT | O_WRONLY, 0644)) == 0);
        const QString link = dir.filePath(QStringLiteral("good"));
        QVERIFY(::symlink("bad\xff", QFile::encodeName(link).constData()) == 0);
        QString error;
        omafiles::PathState state = omafiles::PathState::Present;
        bool unnameable = false;
        QVERIFY(omafiles::realPath(link, &error, &state, &unnameable).isEmpty());
        QVERIFY(unnameable);
        QCOMPARE(state, omafiles::PathState::Unknown);
        QVERIFY2(error.contains(QStringLiteral("bad\\xff")), qPrintable(error));
        QVERIFY(omafiles::canonicalPath(link, nullptr).isEmpty());
    }

    // A row whose own path is now a symlink is Missing, live or dangling.
    void readsALinkAtARowAsMissing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString target = dir.filePath(QStringLiteral("b"));
        const QString row = dir.filePath(QStringLiteral("a"));
        QFile file(target);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QCOMPARE(omafiles::rowState(target, nullptr), omafiles::PathState::Present);
        QVERIFY(QFile::link(target, row));
        QString error;
        QCOMPARE(omafiles::rowState(row, &error), omafiles::PathState::Missing);
        QVERIFY2(error.contains(QStringLiteral("symlink")), qPrintable(error));
    }

    // A kept file that is a fifo is refused at once; a named stream is read.
    void opensAKeptFileWithoutBlocking()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fifo = dir.filePath(QStringLiteral("fifo"));
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QFile kept(fifo);
        QString error;
        QCOMPARE(omafiles::openToRead(&kept, {}, &error), omafiles::PathState::Unknown);
        QVERIFY2(error.contains(QStringLiteral("not a regular file")), qPrintable(error));
        std::thread writer([&fifo] {
            QFile end(fifo);
            if (end.open(QIODevice::WriteOnly))
                end.write("x");
        });
        QFile stream(fifo);
        QCOMPARE(omafiles::openToRead(&stream, {}, &error, omafiles::Reading::Stream),
                 omafiles::PathState::Present);
        writer.join();
        QCOMPARE(stream.readAll(), QByteArray("x"));
    }

    // `mkdir -p` names the component the system refused, with its reason.
    void makesAPathOrSaysWhichPartWouldNot()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString deep = dir.filePath(QStringLiteral("a/b/c"));
        QString error;
        QVERIFY2(omafiles::makePath(deep, &error), qPrintable(error));
        QVERIFY(QFileInfo(deep).isDir());
        const QString blocked = dir.filePath(QStringLiteral("f"));
        QFile file(blocked);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(!omafiles::makePath(blocked + QStringLiteral("/x/y"), &error));
        QCOMPARE(error, blocked + QStringLiteral("/x: ")
                            + QString::fromLocal8Bit(strerror(ENOTDIR)));
    }

    // `HOME` deciding a base directory has to be absolute.
    void refusesARelativeHomeThatDecides()
    {
        const QByteArray home = qgetenv("HOME");
        const QByteArray data = qgetenv("XDG_DATA_HOME");
        qputenv("HOME", "rel");
        QString error;
        const bool decidedByData = omafiles::environmentNameable(&error);
        qputenv("XDG_DATA_HOME", "");
        const bool decidedByHome = omafiles::environmentNameable(&error);
        qputenv("HOME", home);
        qputenv("XDG_DATA_HOME", data);
        QVERIFY(decidedByData);
        QVERIFY(!decidedByHome);
        QVERIFY2(error.contains(QStringLiteral("HOME is not an absolute path")),
                 qPrintable(error));
    }

    // A filesystem with no `user.*` namespace holds no value, so removing one
    // refuses nothing: procfs answers ENOTSUP to the remove on `comm`, and
    // EACCES to it, then ENOTSUP to the read, on the read-only `cmdline`.
    void removesNoValueWhereNoneCanBe()
    {
        for (const char *path : {"/proc/self/comm", "/proc/self/cmdline"}) {
            QString warning;
            QVERIFY2(omafiles::writeTagsXattr(QString::fromLatin1(path), {}, &warning),
                     qPrintable(warning));
            QVERIFY(warning.isEmpty());
        }
    }

    // A root at `/` keeps the whole path below it, and gives it back.
    void splitsAPathUnderARootAtTheTop()
    {
        QCOMPARE(omafiles::relativeTo(QStringLiteral("/tmp/x"), QStringLiteral("/")),
                 QStringLiteral("tmp/x"));
        QCOMPARE(omafiles::relativeTo(QStringLiteral("/a/b/c"), QStringLiteral("/a")),
                 QStringLiteral("b/c"));
        QCOMPARE(omafiles::relativeTo(QStringLiteral("/a"), QStringLiteral("/a")),
                 QStringLiteral("."));
        const QVector<omafiles::Root> before = omafiles::setRoots(
            {omafiles::Root{QStringLiteral("top"), QStringLiteral("/"), false, {}}});
        QString id;
        QString relative;
        omafiles::splitByRoot(QStringLiteral("/tmp/x"), &id, &relative);
        const QString joined = omafiles::joinRoot(id, relative);
        omafiles::setRoots(before);
        QCOMPARE(id, QStringLiteral("top"));
        QCOMPARE(relative, QStringLiteral("tmp/x"));
        QCOMPARE(joined, QStringLiteral("/tmp/x"));
    }

    // A kept file is written where its symlink leads, keeps its mode, and is
    // refused, untouched, where it may not be written, with the system's reason.
    void writesAKeptFileAsQSaveFileDoes()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QDir dir(home.path());
        const QString real = dir.filePath(QStringLiteral("real.jsonl"));
        const QString link = dir.filePath(QStringLiteral("link.jsonl"));
        QString why;
        QVERIFY2(omafiles::writeKeptFile(real, "one\n", &why), qPrintable(why));
        const mode_t mask = ::umask(0);
        ::umask(mask);
        struct stat st {};
        QCOMPARE(::stat(QFile::encodeName(real).constData(), &st), 0);
        QCOMPARE(st.st_mode & 07777, 0666 & ~mask);
        QVERIFY(QFile::link(real, link));
        QVERIFY(QFile::setPermissions(real,
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup));
        QVERIFY2(omafiles::writeKeptFile(link, "two\n", &why), qPrintable(why));
        QVERIFY(QFileInfo(link).isSymLink());
        QFile read(real);
        QVERIFY(read.open(QIODevice::ReadOnly));
        QCOMPARE(read.readAll(), QByteArray("two\n"));
        QCOMPARE(::stat(QFile::encodeName(real).constData(), &st), 0);
        QCOMPARE(st.st_mode & 07777, 0640u);
        QCOMPARE(dir.entryList(QDir::Files | QDir::System).size(), 2);

        if (::geteuid() == 0)
            return;
        QVERIFY(QFile::setPermissions(real, QFile::ReadOwner));
        QVERIFY(!omafiles::writeKeptFile(real, "three\n", &why));
        QCOMPARE(why, omafiles::systemReason(EACCES));
        QVERIFY(read.seek(0));
        QCOMPARE(read.readAll(), QByteArray("two\n"));
    }

    // An index that may not be written is refused when the lock is taken,
    // before any verb acts on a file.
    void refusesAnIndexItCouldNotReplace()
    {
        if (::geteuid() == 0)
            QSKIP("root writes any file");
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString index = QDir(home.path()).filePath(QStringLiteral("index.jsonl"));
        QFile file(index);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
        QVERIFY(QFile::setPermissions(index, QFile::ReadOwner));
        omafiles::Store store(index);
        QString error;
        QVERIFY(!store.lockAndLoad(&error));
        QVERIFY(!store.holdsLock());
        QCOMPARE(error, QStringLiteral("store: cannot write %1: %2")
                            .arg(index, omafiles::systemReason(EACCES)));
    }

    // A pipe reached through `/proc/<pid>/fd` is there, with no name.
    void readsAPipeByDescriptorAsNoName()
    {
        int ends[2];
        QCOMPARE(::pipe(ends), 0);
        omafiles::PathState state = omafiles::PathState::Present;
        QString error;
        const QString path = QStringLiteral("/proc/self/fd/%1").arg(ends[0]);
        const QString resolved = omafiles::realPath(path, &error, &state);
        ::close(ends[0]);
        ::close(ends[1]);
        QVERIFY(resolved.isEmpty());
        QCOMPARE(state, omafiles::PathState::Unknown);
        QCOMPARE(error, path + QStringLiteral(" is a pipe with no name on a filesystem"));
    }

    // Only a space nothing else spells is quoted at an end: CR is `\x0d`.
    void quotesOnlyASpaceItWouldNotShow()
    {
        QCOMPARE(omafiles::spelled(QStringLiteral("x\r")), QStringLiteral("x\\x0d"));
        QCOMPARE(omafiles::spelled(QStringLiteral("\tx")), QStringLiteral("\\tx"));
        QCOMPARE(omafiles::spelled(QStringLiteral("x ")), QStringLiteral("'x '"));
        QCOMPARE(omafiles::spelled(QString(u"x\u00a0")), QString(u"'x\u00a0'"));
    }

    // A value is split at its comma bytes before any word is decoded, so one
    // word that is not UTF-8 leaves its neighbours tags.
    void splitsAValueBeforeDecodingIt()
    {
        const QStringList words = omafiles::tagsOfValue(QByteArray("foo,\xff" "a"));
        QCOMPARE(words.size(), 2);
        QCOMPARE(words.at(0), QStringLiteral("foo"));
        QCOMPARE(omafiles::escapedWords(words.at(1)), QStringLiteral("\\xffa"));
    }

    // xattr(7): a fifo holds no `user.*` value, which is no namespace.
    void readsAFifoAsHoldingNoValue()
    {
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const QString fifo = QDir(home.path()).filePath(QStringLiteral("p"));
        QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QByteArray value;
        QCOMPARE(omafiles::readTagsValue(fifo, &value, nullptr), omafiles::TagsValue::Unsupported);
    }
};

QTEST_MAIN(StoreTest)
#include "tst_store.moc"
