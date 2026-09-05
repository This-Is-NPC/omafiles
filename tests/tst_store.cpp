#include <QtTest>

#include "Paths.h"
#include "Record.h"
#include "Roots.h"
#include "Store.h"
#include "Tag.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
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

private slots:
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
        store.put(omafiles::Record{record.path, {}, 0});
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

    // A row with no tags is not a row. Two places drop it -- `put` when the
    // last tag comes off, and `load` when one turns up in the file anyway --
    // and neither can be caught alone, because disabling either leaves the
    // other covering for it.
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

        // And one that reaches the file some other way -- a hand-edited dump,
        // an `import` -- does not come back in.
        {
            QFile handle(index);
            QVERIFY(handle.open(QIODevice::Append | QIODevice::Text));
            handle.write("{\"path\":\"/tmp/tagless.md\",\"tags\":[]}\n");
        }
        omafiles::Store reread(index);
        QVERIFY(reread.load(&error));
        QVERIFY2(!reread.contains(QStringLiteral("/tmp/tagless.md")),
                 "a tagless row in the file was loaded as a row");
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

    // The bug the whole feature exists for: an unmounted tree is `ENOENT` from
    // a stat, which is exactly what every file in it having been deleted looks
    // like -- and `prune` acts on that answer.
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
};

QTEST_MAIN(StoreTest)
#include "tst_store.moc"
