#include <QtTest>

#include "Paths.h"
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
};

QTEST_MAIN(StoreTest)
#include "tst_store.moc"
