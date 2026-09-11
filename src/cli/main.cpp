#include "Names.h"
#include "Open.h"
#include "Paths.h"
#include "Roots.h"
#include "View.h"
#include "Store.h"
#include "Tag.h"
#include "Xattr.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <cstdio>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>

using namespace omafiles;

namespace {

constexpr int kOk = 0;
constexpr int kUsage = 1;
constexpr int kPathMissing = 2;
constexpr int kStore = 3;

// A list the system would not let this run finish.
//
// The same 1 as any other "no": a list missing the rows the system declined to
// describe is not the answer that was asked for, and `omafiles ls > full-disk`
// exiting 0 tells a build system that a truncated list is whole. Taken by the
// verbs whose whole output is an answer -- `ls`, `tags`, `recent`. `prune`'s
// output is a receipt for what it dropped, so a row it stepped over leaves the
// run complete; `verify` reports one as a problem and exits 1 through that.
constexpr int kIncomplete = 1;

// What a path the run could not use costs the exit code.
//
// 2 is a path that is not there. Anything else -- a path the system will not
// describe, a file it will not open -- is a path nobody may look at, which is
// an answer the run could not finish and so the 1 above. One table, so an
// operand, a `[dir]` and a dump file cannot answer three different ways for the
// same chmod.
int pathStatus(PathState state)
{
    return state == PathState::Missing ? kPathMissing : kIncomplete;
}

struct Globals {
    bool json = false;
    /// Whether `--json` was typed, as against defaulted from `OMAFILES_JSON`.
    /// Only `completions` asks: it refuses the flag, and refusing it over an
    /// environment default would break `source <(omafiles completions bash)` in
    /// every shell that exports one.
    bool jsonAsked = false;
    bool dryRun = false;
    bool yes = false;
    bool quiet = false;
    bool help = false;
    bool version = false;
};

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

bool envFlag(const char *name)
{
    const QByteArray value = qgetenv(name);
    return value == "1" || value.compare("true", Qt::CaseInsensitive) == 0
        || value.compare("yes", Qt::CaseInsensitive) == 0;
}

bool isGlobalFlag(const QString &arg)
{
    return arg == QLatin1String("-j") || arg == QLatin1String("--json")
        || arg == QLatin1String("-n") || arg == QLatin1String("--dry-run")
        || arg == QLatin1String("-y") || arg == QLatin1String("--yes")
        || arg == QLatin1String("-q") || arg == QLatin1String("--quiet")
        || arg == QLatin1String("-h") || arg == QLatin1String("--help")
        || arg == QLatin1String("-V") || arg == QLatin1String("--version");
}

// Which flags take the next word as their value, anywhere in the program.
//
// POSIX Utility Syntax Guideline 6: the token following an option that takes an
// argument is that argument, taken literally. The globals are stripped in a
// pass before the verb's own parse, so without this `ls --ext -j sub` reads
// `-j` as `--json` and `--ext` eats the `[dir]`.
//
// Every value-taking flag, not the ones the verb at hand has: this pass does
// not know the verb yet, and `parseFlags` refuses a flag its verb does not
// take, so the two scans never disagree about which words are values.
bool takesValue(const QString &arg)
{
    return arg == QLatin1String("-t") || arg == QLatin1String("--tag")
        || arg == QLatin1String("--any") || arg == QLatin1String("--ext")
        || arg == QLatin1String("--since") || arg == QLatin1String("--from")
        || arg == QLatin1String("--id") || arg == QLatin1String("--check")
        || arg == QLatin1String("--name") || arg == QLatin1String("--replace")
        || arg == QLatin1String("--with") || arg == QLatin1String("--pattern");
}

void applyGlobal(Globals *g, const QString &arg)
{
    if (arg == QLatin1String("-j") || arg == QLatin1String("--json")) {
        g->json = true;
        g->jsonAsked = true;
    } else if (arg == QLatin1String("-n") || arg == QLatin1String("--dry-run"))
        g->dryRun = true;
    else if (arg == QLatin1String("-y") || arg == QLatin1String("--yes"))
        g->yes = true;
    else if (arg == QLatin1String("-q") || arg == QLatin1String("--quiet"))
        g->quiet = true;
    else if (arg == QLatin1String("-h") || arg == QLatin1String("--help"))
        g->help = true;
    else if (arg == QLatin1String("-V") || arg == QLatin1String("--version"))
        g->version = true;
}

void warn(const Globals &g, const QString &message)
{
    if (g.quiet)
        return;
    err() << message << '\n';
}

void fail(const QString &message)
{
    err() << message << '\n';
}

// The verb as a diagnostic names it: the first word that is not a flag, and
// the subcommand too where the verb has them. Empty when no verb was named.
QString spokenVerb(const QStringList &words)
{
    QString verb;
    for (const QString &word : words) {
        if (word.startsWith(QLatin1Char('-')))
            continue;
        verb = verb.isEmpty() ? word : verb + QLatin1Char(' ') + word;
        if (verb != QLatin1String("tag"))
            break;
    }
    return verb;
}

// A write that failed is not a run that succeeded.
//
// stdout is a buffer, so the last `out() <<` can land in it and the flush that
// empties it fail; the check belongs after the flush, at exit, not after each
// write -- coreutils spells it that way in `close_stdout`. Exit 1, which is
// already this program's "it ran and the answer is no".
//
// Named by the verb that was writing, like every other diagnostic here and like
// coreutils' own `ls: write error`.
int finished(const QString &verb, int status)
{
    out().flush();
    // Both halves: QTextStream writes into stdio, and stdio writes to the fd,
    // so a full disk can surface at either one.
    const bool broken = out().status() != QTextStream::Ok || std::fflush(stdout) != 0;
    if (!broken)
        return status;
    const QString reason = QString::fromLocal8Bit(std::strerror(errno));
    fail(verb.isEmpty() ? QStringLiteral("write error: %1").arg(reason)
                        : QStringLiteral("%1: write error: %2").arg(verb, reason));
    return status == kOk ? 1 : status;
}

void printJson(const QJsonValue &value)
{
    const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray())
                                               : QJsonDocument(value.toObject());
    out() << QString::fromUtf8(doc.toJson(QJsonDocument::Compact)) << '\n';
}

// The whole help, or one verb's part of it. The lines are indexed by the verb
// they start with, and a line indented past its own is a continuation of it,
// which is what makes the two-line entries for `ls` and `prune` come out
// whole.
bool printHelp(const QString &verb = QString())
{
    // The version is spliced in rather than written here: a raw string cannot
    // interpolate, and a second copy of the number is a second thing to forget.
    static const QString body = QStringLiteral(R"(Tag files and list them by tag

Usage: omafiles <command> [options]

Globals: --json  --dry-run  --yes  --quiet

Tags:
  tag add    --tag <tag>... -- <path>...
  tag rm     --tag <tag>... -- <path>...
  tag ls     -- <path>
  tag clear  -- <path>...
  tag cp     --from <path> -- <path>...   adds them, keeps what is there
  tag merge  <from> <to>            (alias: tag rename)

Asking:
  ls       [--tag <tag>]... [--any <tag>]... [--untagged]
           [--cached] [--ext <ext>]... [--since <duration>] [dir]
                                    --since counts from when it was tagged
                                    --cached answers --untagged out of the
                                    list sweep built, and says its age
  tags     [dir]
  has      --tag <tag> -- <path>    exit 0 if tagged, 1 if not
  json     -- <path>
  recent   [n]                      last tagged first
  export                            one index row per line, as jsonl: many
                                    documents, which a JSON parser refuses
                                    --json instead dumps them as one array
  import   <file>                   merge a dump into the index
                                    - reads stdin, the way batch does

Moving:
  open     <path>...                what the desktop would use, or xdg-open
  mv       <old> <new>              file and its tags; a folder cannot
                                    cross a filesystem, and neither verb
                                    writes over what is already there
                                    --replace <text> --with <text> and
                                    --pattern <template> instead rename a
                                    set by rule, each in its own folder:
                                    {name} {stem} {ext} {n} -- <path>...
  cp       <old> <new>              file and its tags; will not write over
                                    what is already there
  trash    <path>...                gio trash, then drop the rows
  rm       <path>...                delete, then drop the rows
                                    both refuse the whole list over an
                                    operand they cannot take

Saved:
  view ls                           the saved views: name, then what it asks
                                    `view` alone prints it too
  view add --name <name>            save one: a set of tags, or a folder
           [--tag <tag>]... [dir]   a view is one or the other, never both
  view rm  --name <name>

Keeping it true:
  roots ls                          the declared roots, and whether each is
                                    here; `roots` alone prints it too
  roots add --id <id> [--mount]     declare one; --check <path> proves it is
            [--check <p>] <path>    mounted more surely than the mount point
  roots rm  --id <id>               its rows come with it, as absolute paths
  roots migrate                     rewrite every row in the form roots give it
                                    a row on a declared root that is not here
                                    is unreachable, not missing: prune and
                                    verify both step over it
  prune    [dir] [--all]            drop rows whose file is gone (asks first)
                                    a directory, or --all for the whole index
  reindex  [--from-xattr] [dir]     rebuild rows from the files' own tags
                                    exit 1 if it had to walk away from a path
  sweep    [--status] [dir]         the run you do not watch: adopt what the
                                    files carry, count what is gone, drop
                                    nothing; the declared roots by default
                                    --status prints the last run's receipt
  verify   [dir]                    exit 1 if index and disk disagree
                                    walks the current directory by default

Many at once:
  batch    [file]                   one command per line, one store write
                                    no nesting, no read verbs, no rm, mv, cp
                                    or trash, and the globals go before it

Shell:
  completions <bash|zsh|fish>       source it, or install it once
                                    the one verb with no --json shape
)");

    if (verb.isEmpty()) {
        out() << "omafiles " OMAFILES_VERSION "\n" << body;
        return true;
    }

    // Where every description in the body starts. Two entries in the `tag`
    // block run their arguments past it, so the column alone cannot find the
    // split; and `export` takes no arguments at all, so the first gap after its
    // name is its description rather than its shape. Skipping the padding after
    // the name and then splitting on the next gap gets both, with the column
    // asked only whether there is a shape there to find.
    constexpr int kDescriptionColumn = 36;
    // The arguments an entry names, or nothing when it names none.
    const auto synopsisOf = [](const QString &line, int afterName) -> QString {
        int start = afterName;
        while (start < line.size() && line.at(start) == QLatin1Char(' '))
            ++start;
        if (start >= line.size() || start >= kDescriptionColumn)
            return {};
        const int gap = line.indexOf(QStringLiteral("  "), start);
        return (gap < 0 ? line.mid(start) : line.mid(start, gap - start)).trimmed();
    };

    QStringList lines;
    // The shape the usage line carries. `ls` wraps its flags onto a second
    // line, so the synopsis is gathered from every line left of the description
    // column, not just the first.
    QStringList shape;
    int entries = 0;
    bool taking = false;
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        const bool entry = line.startsWith(QLatin1String("  "))
            && !line.startsWith(QLatin1String("      "));
        // A line at the left margin is a section heading, and it ends whatever
        // came before it, so the last entry of a section does not drag the next
        // heading along.
        if (!entry && !line.startsWith(QLatin1Char(' ')) && !trimmed.isEmpty())
            taking = false;
        if (entry) {
            // `tag add` is spelled in two words and `tag` alone names all six.
            const QStringList words = trimmed.split(QLatin1Char(' '),
                                                    Qt::SkipEmptyParts);
            const QString named = words.value(0) == QLatin1String("tag")
                ? QStringLiteral("tag ") + words.value(1)
                : words.value(0);
            // An entry names its own aliases, so the spelling the body does
            // not start with is still findable -- `tag rename` for
            // `tag merge`.
            taking = named == verb || words.value(0) == verb
                || trimmed.contains(QStringLiteral("(alias: %1)").arg(verb));
            if (taking) {
                ++entries;
                int start = 0;
                while (start < line.size() && line.at(start) == QLatin1Char(' '))
                    ++start;
                shape.append(synopsisOf(line, start + named.size()));
            }
        } else if (taking && !trimmed.isEmpty()
                   && line.size() - trimmed.size() < kDescriptionColumn) {
            // A continuation left of the description column is more synopsis;
            // one at it is prose about the entry above.
            shape.append(synopsisOf(line, 0));
        }
        if (taking && !trimmed.isEmpty())
            lines.append(line);
    }

    // Nothing to say about it, because it is not a verb. The caller decides
    // what that means.
    if (lines.isEmpty())
        return false;
    out() << QStringLiteral("Usage: omafiles ") << verb;
    // `omafiles tag --help` names all six subcommands, and six synopses run
    // together is not one usage line; each entry below carries its own.
    if (entries == 1) {
        for (const QString &part : shape) {
            if (!part.isEmpty())
                out() << ' ' << part;
        }
    }
    out() << '\n' << '\n';
    for (const QString &line : lines)
        out() << line << '\n';
    // `completions` prints a shell script and refuses --json, so its own help
    // must not offer the flag two lines under the sentence that rejects it.
    out() << (verb == QLatin1String("completions")
                  ? QStringLiteral("\nGlobals: --dry-run  --yes  --quiet\n")
                  : QStringLiteral("\nGlobals: --json  --dry-run  --yes  --quiet\n"))
          << QStringLiteral("Full reference: docs/cli.md\n");
    return true;
}

bool consumeValue(QStringList &args, int *i, QString *value)
{
    if (*i + 1 >= args.size())
        return false;
    *value = args.at(*i + 1);
    ++*i;
    return true;
}

// Which flags this verb has. A flag accepted where it means nothing is a flag
// silently ignored, so `ls --from-xattr` is a typo rather than a no-op.
//
// It sits up here because `parseFlags` is not the only reader: completion
// offers the flags a word can be, and a second table would be free to drift
// from the one the parse enforces.
struct Takes {
    bool tag = false;
    bool lsFlags = false;
    bool from = false;
    bool fromXattr = false;
    bool all = false;
    bool root = false;
    bool view = false;
    bool status = false;
    /// `--replace`, `--with` and `--pattern`: `mv` over a set, by rule.
    bool rename = false;
};

// The six `tag` subcommands, each with its own flags. `dispatch` gates the
// parse on this and completion offers from it, so a flag can only be suggested
// where it will be taken.
Takes tagTakes(const QString &sub)
{
    if (sub == QLatin1String("add") || sub == QLatin1String("rm"))
        return {.tag = true};
    if (sub == QLatin1String("cp"))
        return {.from = true};
    return {};                                   // ls, clear, merge, rename
}

// A number that does not fit is rejected, not folded into one that does.
//
// `toLongLong` with no `ok` pointer returns 0 on overflow, which would turn the
// widest window anybody can type into `--since 0s`; and `n * scale` overflows
// qint64 on its own, which is undefined behaviour rather than a large number.
// Go's `time.ParseDuration` and systemd's `parse_sec` both refuse what they
// cannot hold instead of saturating.
qint64 parseDuration(const QString &text, QString *error)
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)([smhd])$"));
    const QRegularExpressionMatch match = re.match(text);
    // One message: a duration nobody can represent is as unusable as one nobody
    // can parse, and the shape it should have had is the useful half of both.
    *error = QStringLiteral("invalid duration: %1 (use e.g. 7d, 12h, 30m, 90s)").arg(text);
    if (!match.hasMatch())
        return -1;
    bool ok = false;
    const qint64 n = match.captured(1).toLongLong(&ok);
    if (!ok)
        return -1;
    const QChar unit = match.captured(2).at(0);
    qint64 scale = 86400;
    if (unit == QLatin1Char('s'))
        scale = 1;
    else if (unit == QLatin1Char('m'))
        scale = 60;
    else if (unit == QLatin1Char('h'))
        scale = 3600;
    if (n > std::numeric_limits<qint64>::max() / scale)
        return -1;
    error->clear();
    return n * scale;
}

// A span of seconds in the spelling `--since` accepts, so the one vocabulary
// covers both saying an age and asking about one.
QString spelledDuration(qint64 seconds)
{
    if (seconds >= 86400)
        return QStringLiteral("%1d").arg(seconds / 86400);
    if (seconds >= 3600)
        return QStringLiteral("%1h").arg(seconds / 3600);
    if (seconds >= 60)
        return QStringLiteral("%1m").arg(seconds / 60);
    return QStringLiteral("%1s").arg(qMax<qint64>(seconds, 0));
}

QString normalizeExt(QString ext)
{
    if (ext.startsWith(QLatin1Char('.')))
        ext = ext.mid(1);
    return ext.toLower();
}

// Everything a walk of the disk must offer to a verb that works on tags.
//
// Directories carry tags like files do -- `tag add -- somedir` is documented --
// so a walk that asks for `QDir::Files` cannot see a tagged directory at all.
// Hidden entries count for the same reason: a dotfile is a file somebody chose
// to tag.
constexpr QDir::Filters kTaggableEntries =
    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden;

// The paths in one directory that no path here can spell -- see
// `readDirectory`, which is where the bytes are read and the mangling happens.
void unnameableEntries(const QString &directory, QStringList *names)
{
    QStringList mangled;
    if (!readDirectory(directory, nullptr, &mangled, nullptr))
        return;
    for (const QString &spelled : std::as_const(mangled))
        names->append(directory + QLatin1Char('/') + spelled);
}

// Every path under `root` that a tag-aware verb has to consider.
//
// `withRoot` because `QDirIterator` enumerates what is inside a directory and
// never the directory itself, and `verify somedir` has to check the thing it
// was pointed at. `ls` does not want it: you asked what is in the folder, not
// about the folder.
//
// Canonical, and each path once. Two entries in a walk can be the same file --
// a symlink and its target both turn up -- and a walk that does not notice
// reports one problem twice, or writes two index rows for one inode which
// `tag ls` can then only find under one of them.
QStringList tagWalk(const QString &root, bool withRoot, QStringList *unreadable = nullptr,
                    QStringList *unnamed = nullptr)
{
    QString base = canonicalPath(root, nullptr);
    if (base.isEmpty())
        base = root;

    QStringList found;
    QSet<QString> seen;
    // The checks every entry below gets, hoisted so the root can have them too.
    const auto note = [&](const QString &path) {
        const QFileInfo info(path);
        if (!info.isDir())
            return;
        if (unreadable && !info.isReadable())
            unreadable->append(path);
        // Only where the walk itself goes: it does not follow a symlink into
        // another directory, so neither does this.
        if (unnamed && info.isReadable() && !info.isSymLink())
            unnameableEntries(path, unnamed);
    };
    // The named directory is checked whether or not it is listed.
    // `QDirIterator` never yields the directory it was pointed at, so without
    // this a root nobody may open reads as "there is nothing untagged here"
    // rather than "I could not look". /usr/bin/ls answers `cannot open
    // directory` and exits 2, /usr/bin/find `Permission denied` and exits 1;
    // neither reports an empty directory.
    if (!withRoot)
        note(base);
    const auto take = [&](const QString &path) {
        QString canonical = canonicalPath(path, nullptr);
        if (canonical.isEmpty())
            canonical = path;
        // A symlink resolves to wherever it points, and that can be outside
        // the tree entirely. A walk of one directory that answers about another
        // is not a walk of that directory: a row written outside the root is
        // one the next `verify` filters back out and reports all over again.
        if (!isUnderDirectory(canonical, base))
            return;
        // And when the root was not asked for it stays out however it is
        // reached: a symlink inside the tree pointing back at the tree
        // canonicalises onto the root.
        if (!withRoot && canonical == base)
            return;
        if (seen.contains(canonical))
            return;
        seen.insert(canonical);
        // A directory nobody may open is not an empty directory:
        // `QDirIterator` walks past it without a word, and silence from a verb
        // whose job is to say whether two things agree reads as agreement.
        note(canonical);
        found.append(canonical);
    };
    if (withRoot)
        take(root);
    QDirIterator walk(root, kTaggableEntries, QDirIterator::Subdirectories);
    while (walk.hasNext())
        take(walk.next());
    return found;
}

// Which verbs only answer questions.
//
// A reader does not take the lock: the commit lands by rename, so it sees the
// old index or the new one and never half of either, and making it queue behind
// a writer would buy nothing. A writer takes the lock before it reads.
bool onlyReads(const QString &verb)
{
    static const QStringList reads = {QStringLiteral("ls"),     QStringLiteral("tags"),
                                      QStringLiteral("has"),    QStringLiteral("json"),
                                      QStringLiteral("export"), QStringLiteral("verify"),
                                      QStringLiteral("open"),   QStringLiteral("recent"),
                                      QStringLiteral("completions"),
                                      QStringLiteral("__complete")};
    return reads.contains(verb);
}

// Which store a verb works on, and who decides when it lands.
//
// A verb run on its own owns both halves: it loads the index, mutates it and
// commits before it returns. Run from `batch` a dozen verbs share one session
// and the commit belongs to batch, so the jsonl is read once and written once,
// and a line that fails takes the whole run down with it because nothing had
// reached the disk yet.
struct Session {
    Store store{defaultIndexPath()};
    bool opened = false;
    /// Set from the verb before anything opens the store. A writer holds the
    /// lock from before its read until after its commit.
    bool writes = true;
    bool deferred = false;    // batch commits, and speaks, for the whole run
    QVector<Record> written;  // rows put into the store, in the order they were put
    QVector<Record> pending;  // rows whose xattr is waiting on that commit
    /// The pending paths whose copy is part of what the verb promised, so the
    /// flush can tell them from the ones a refusal costs nothing.
    ///
    /// `reindex` exits 1 over a refused write and `tag add` exits 0 over the
    /// identical one: `tag add` promises the tag is recorded and the row is the
    /// record, while `reindex` promises the index and the files agree about the
    /// tree it walked. That is also which stream says so -- see `flushPending`.
    QSet<QString> mustMirror;
    /// Whether the run walked away from something it was asked to settle. The
    /// rows it wrote stand -- this is a diagnosis and not a failed write -- so
    /// the verb exits 1 with its answer printed, and a `batch` reports it after
    /// its own commit rather than rolling back.
    ///
    /// Every message that comes with one goes through `fail` and not `warn`:
    /// `--quiet` is "No stderr except errors", and a path a run declined to
    /// settle is why it exits 1, not a note about it.
    bool incomplete = false;
};

// Read the index the first time a verb asks for it and not again. Twenty
// `tag add` lines rereading and rewriting the same jsonl twenty times is the
// cost `batch` exists to avoid, and it is only avoided if the load is shared.
int openSession(Session *session)
{
    if (session->opened)
        return kOk;
    QString error;
    const bool ok = session->writes ? session->store.lockAndLoad(&error)
                                    : session->store.load(&error);
    if (!ok) {
        fail(error);
        return kStore;
    }
    session->opened = true;
    return kOk;
}

// What a run of many writes ended up with, for the paths it touched: each path
// once, in the order it was first touched, and the row the index holds for it
// now. A path the run wrote twice is one row rather than two states it passed
// through, and a path it wrote and then cleared is no row at all, because the
// commit kept none.
//
// For the two verbs whose one answer covers many writes -- `batch` over its
// lines, `import` over its dump. A verb run on its own reports the row it
// wrote, emptied or not: that is its receipt and not a summary of a run.
QVector<Record> committedRows(const Store &store, const QVector<Record> &touched)
{
    QVector<Record> rows;
    QSet<QString> seen;
    for (const Record &record : touched) {
        if (seen.contains(record.path))
            continue;
        seen.insert(record.path);
        const Record kept = store.get(record.path);
        if (!kept.path.isEmpty())
            rows.append(kept);
    }
    return rows;
}

// `announce` is false for the two verbs that print their own answer. `mv` and
// `cp` say where the file went, and a row array emitted beside that object
// would be a second document on the same stdout.
int commitStore(const Globals &g, Store *store, const QVector<Record> &written,
                bool announce = true)
{
    if (g.dryRun) {
        if (!announce)
            return kOk;
        if (g.json) {
            QJsonArray array;
            for (const Record &record : written)
                array.append(record.toJson());
            printJson(array);
        } else {
            for (const Record &record : written)
                out() << record.path << '\n';
        }
        return kOk;
    }
    QString error;
    if (!store->commit(&error)) {
        fail(error);
        return kStore;
    }
    if (announce && g.json) {
        QJsonArray array;
        for (const Record &record : written)
            array.append(record.toJson());
        printJson(array);
    }
    return kOk;
}

// The files' own copies of their tags, written once the rows they mirror are
// certainly on disk. Counts the ones the filesystem refused into the session,
// for the verb that has to say whether it settled what it walked.
//
// The refusal goes out on the stream the verb's own exit code makes it. Where
// the copy is part of what was promised the run exits 1 and the path is why, so
// it is an error and `--quiet` keeps it. Where it is not, the run exits 0 -- the
// row is the record -- and a note beside a success is exactly what `--quiet`
// means by "No stderr except errors". See `Session::mustMirror`.
void flushPending(const Globals &g, Session &s)
{
    for (const Record &record : s.pending) {
        QString warning;
        if (writeTagsXattr(record.path, record.tags, &warning))
            continue;
        const bool promised = s.mustMirror.contains(record.path);
        if (!warning.isEmpty()) {
            if (promised)
                fail(warning);
            else
                warn(g, warning);
        }
        if (promised)
            s.incomplete = true;
    }
    s.pending.clear();
    s.mustMirror.clear();
}

// Hand the rows a verb wrote to whoever owns the commit. Inside a batch that is
// batch itself, which is also why nothing is printed here: a line that
// announced its write and a later line that failed would have announced rows
// that were rolled back.
int finish(const Globals &g, Session &s, const QVector<Record> &written,
           bool announce = true)
{
    s.written.append(written);
    if (s.deferred)
        return kOk;
    const int status = commitStore(g, &s.store, written, announce);
    if (status != kOk)
        return status;
    flushPending(g, s);
    return kOk;
}

// The file's own copy of its tags, held back until the row that mirrors it has
// landed. The commit can still fail -- a read-only index, a full disk, a lock
// it never got -- and a file claiming tags no row records is exactly the
// disagreement `verify` exists to shout about.
//
// A dry run does not do it, and does not predict it either. A dry run makes
// every refusal this program decides for itself, because each of those is a
// fact about a path or about PATH, there to be read; whether a filesystem will
// take a `user.*` write is not, and every proxy is wrong in both directions --
// `access(W_OK)` says no for root, and yes for a mount or an immutable flag
// that will refuse it anyway. So `reindex --dry-run` on a chmod 444 file prints
// the path and exits 0 where the real run exits 1.
// `required` for the callers whose promise the file's copy is part of: see
// `Session::mustMirror`.
void mirrorTags(const Globals &g, Session &s, const Record &record, bool required = false)
{
    if (g.dryRun)
        return;
    s.pending.append(record);
    if (required)
        s.mustMirror.insert(record.path);
}

// Handed in as the carried stamp by a write that changed no tag on a row that
// has none: the row stays undated. Zero cannot say it -- zero is what an
// undated row already carries, and `stamped` reads that as "nothing to carry"
// and falls back to now.
constexpr qint64 kKeepUndated = -1;

// Builds the row a write verb is about to store.
//
// `carriedTagged` is for the writes that must not reset the clock: moving a
// file is not tagging it, so `mv` hands the old row's stamp back in, and so
// does a write that changed no tag. Everything else -- adding, removing,
// clearing, copying tags onto a path -- is a tagging event and gets now, which
// is the whole basis on which `recent` answers.
//
// `state` is which way the check below went, for the caller that turns it into
// an exit code: every one of them resolved the path already, so a stat that
// fails here is the path going away, or the system going quiet about it,
// between then and now -- and those are still the two different answers.
Record stamped(const QString &path, QStringList tags, QString *error, PathState *state,
               qint64 carriedTagged = 0)
{
    Record record;
    record.path = path;
    record.tags = uniqueSorted(std::move(tags));
    // Asked so the verb fails on a path the system will not talk about, rather
    // than writing a row about it.
    const PathState found = pathState(path, error);
    if (state)
        *state = found;
    if (found != PathState::Present)
        return {};
    record.tagged = carriedTagged == kKeepUndated
        ? 0
        : (carriedTagged > 0 ? carriedTagged : QDateTime::currentSecsSinceEpoch());
    return record;
}

// The stamp a write that changed nothing hands back: the one the row had,
// including none. An undated row carries zero, which `stamped` reads as "no
// stamp to carry", so it takes `kKeepUndated` to stay undated -- otherwise
// `tag rm --tag` naming a tag the file never had sends it to the top of
// `recent`.
qint64 keptStamp(const Record &row)
{
    return row.tagged > 0 ? row.tagged : kKeepUndated;
}

QStringList resolveAll(const QStringList &paths, int *status,
                       QString (*resolve)(const QString &, QString *, PathState *))
{
    QStringList out;
    // Every operand it cannot resolve is named, not just the first, the way
    // coreutils `rm` names each one it cannot remove. The subset it could
    // resolve comes back with the status, so a caller with checks of its own
    // can name their offenders too.
    for (const QString &path : paths) {
        QString error;
        PathState state = PathState::Missing;
        const QString canonical = resolve(path, &error, &state);
        if (canonical.isEmpty()) {
            fail(error);
            // A list holding one of each reports the missing path, because that
            // one is settled. See `pathStatus`.
            *status = qMax(*status, pathStatus(state));
            continue;
        }
        out.append(canonical);
    }
    return out;
}

// Through to whatever the path finally points at.
//
// This is what the tagging verbs want. On Linux a `user.*` xattr cannot be set
// on a symlink at all, so the file at the end of the link is the only thing
// `tag add` could mean, and a row filed under the link's name would be a second
// row for a file that already has one.
QStringList resolveExisting(const QStringList &paths, int *status)
{
    return resolveAll(paths, status, canonicalPath);
}

// The path as named, link and all.
//
// For the verbs that act on the path itself rather than on the bytes at the end
// of it: POSIX `unlink()`, coreutils `rm` and `mv`, and `gio trash` all act on
// the link.
QStringList resolveNamed(const QStringList &paths, int *status)
{
    return resolveAll(paths, status, canonicalLinkPath);
}

int cmdTagAdd(const Globals &g, Session &s, const QStringList &rawTags, const QStringList &paths)
{
    if (rawTags.isEmpty()) {
        fail(QStringLiteral("tag add: --tag is required"));
        return kUsage;
    }
    QString error;
    const QStringList tags = normalizeTags(rawTags, &error);
    if (tags.isEmpty()) {
        fail(error);
        return kUsage;
    }
    if (paths.isEmpty()) {
        fail(QStringLiteral("tag add: path required"));
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QVector<Record> written;
    for (const QString &path : resolved) {
        Record record = s.store.get(path);
        QStringList next = record.tags;
        next.append(tags);
        // Adding a tag the file already carries is not a tagging event, so the
        // row keeps the stamp it had. Everything else still happens: the file's
        // own copy is the half that goes missing on its own -- a `cp` without
        // `--preserve=xattr`, a move onto a filesystem with no `user.*` -- so
        // skipping the write would leave `verify` shouting about a disagreement
        // this verb is the repair for.
        const qint64 carried = uniqueSorted(next) == record.tags ? keptStamp(record) : 0;
        PathState state = PathState::Missing;
        record = stamped(path, next, &error, &state, carried);
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        mirrorTags(g, s, record);
    }
    return finish(g, s, written);
}

int cmdTagRm(const Globals &g, Session &s, const QStringList &rawTags, const QStringList &paths)
{
    if (rawTags.isEmpty()) {
        fail(QStringLiteral("tag rm: --tag is required"));
        return kUsage;
    }
    QString error;
    const QStringList tags = normalizeTags(rawTags, &error);
    if (tags.isEmpty()) {
        fail(error);
        return kUsage;
    }
    if (paths.isEmpty()) {
        fail(QStringLiteral("tag rm: path required"));
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QVector<Record> written;
    for (const QString &path : resolved) {
        const Record existing = s.store.get(path);
        const bool rowless = existing.path.isEmpty();
        Record record = existing;
        QStringList next;
        for (const QString &tag : record.tags) {
            if (!tags.contains(tag))
                next.append(tag);
        }
        // Same as `tag add`: removing a tag that was never on the file changes
        // no tag, so the row keeps its stamp -- and the file's own copy is
        // rewritten anyway, because that is the half that can go missing.
        const qint64 carried = next == record.tags ? keptStamp(record) : 0;
        PathState state = PathState::Missing;
        record = stamped(path, next, &error, &state, carried);
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        // A path the index never heard of is not a row, so an empty `tags`
        // receipt here would invent one. Only paths that had a row get written.
        if (!rowless)
            written.append(record);
        // A drop the filesystem refuses on a path with no row is a drop nothing
        // recorded: exit 0 there would claim a removal that did not happen.
        mirrorTags(g, s, record, rowless);
    }
    if ((status = finish(g, s, written)) != kOk)
        return status;
    return s.incomplete ? kIncomplete : kOk;
}

int cmdTagLs(const Globals &g, Session &s, const QStringList &paths)
{
    if (paths.size() != 1) {
        fail(QStringLiteral("tag ls: one path required"));
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;
    if ((status = openSession(&s)) != kOk)
        return status;
    Record record = s.store.get(resolved.first());
    // A store miss is not a row: empty stdout, exit 0, like plain `tag ls`.
    if (record.path.isEmpty())
        return kOk;
    if (g.json)
        printJson(record.toJson());
    else {
        for (const QString &tag : record.tags)
            out() << tag << '\n';
    }
    return kOk;
}

// What `--ext` asks, asked the same way everywhere.
//
// A directory named `notes.bak` has a suffix the way any other name does, and
// answering an extension query with it answers a different question.
//
// A leading dot names the file rather than introducing an extension, so `--ext
// hidden` does not match `.hidden`, a file whose extension is nothing. Python's
// `os.path.splitext('.cshrc')` returns `('.cshrc', '')` and Go settled it the
// same way (golang/go#24239). `.hidden.txt` still has one.
bool matchesExt(const QString &path, const QStringList &exts)
{
    if (exts.isEmpty())
        return true;
    if (QFileInfo(path).isDir())
        return false;
    const QString name = QFileInfo(path).fileName();
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return false;
    return exts.contains(name.mid(dot + 1).toLower());
}

// `--since` asks when the row was tagged, not when the file was touched: in a
// tag index "since last week" means "what did I file last week", which is a
// different question from the filesystem's mtime.
//
// An undated row cannot answer at all. Its caller decides what to do about it
// -- `ls` counts it and says so, after every other filter -- so this only ever
// sees rows that have a stamp.
bool matchesSince(const Record &record, qint64 sinceSeconds)
{
    if (sinceSeconds < 0)
        return true;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return record.tagged >= now - sinceSeconds;
}

bool matchesTags(const Record &record, const QStringList &ands, const QStringList &anys)
{
    for (const QString &tag : ands) {
        if (!record.tags.contains(tag))
            return false;
    }
    if (anys.isEmpty())
        return true;
    for (const QString &tag : anys) {
        if (record.tags.contains(tag))
            return true;
    }
    return false;
}

QString optionalDir(const QStringList &positionals, int *status)
{
    if (positionals.isEmpty())
        return {};
    if (positionals.size() != 1) {
        fail(QStringLiteral("expected at most one directory"));
        *status = kUsage;
        return {};
    }
    QString error;
    PathState state = PathState::Missing;
    const QString dir = canonicalPath(positionals.first(), &error, &state);
    if (dir.isEmpty()) {
        fail(error);
        // The same table an operand takes: a `[dir]` nobody may look at is not
        // a `[dir]` that is not there. See `pathStatus`.
        *status = pathStatus(state);
        return {};
    }
    if (!QFileInfo(dir).isDir()) {
        fail(QStringLiteral("not a directory: %1").arg(positionals.first()));
        *status = kUsage;
        return {};
    }
    return dir;
}


// `optionalDir` only asks whether the name is a directory. stat still
// answers for a chmod 000, so a verb that then iterates the index without
// opening it would print nothing and exit 0 about a folder nobody may look at.
// `ls --untagged` already catches this through `tagWalk`; these three do not walk.
bool cannotLookInto(const QString &dir)
{
    return !dir.isEmpty() && !QFileInfo(dir).isReadable();
}

// The flags `--untagged` cannot be combined with, and what each pair is
// refused for.
//
// One table: `cmdLs` refuses from it and `__complete` declines to offer from
// it, so completion cannot suggest a flag the verb is about to turn down --
// clap_complete derives exactly this from `conflicts_with`, and git's
// completion keeps the rule by hand. A second list of the same pairs would
// drift away from the refusal.
struct Exclusion {
    QLatin1String flag;
    QLatin1String why;
};
const Exclusion kUntaggedExcludes[] = {
    {QLatin1String("--tag"), QLatin1String("a file with no tags matches no tag")},
    {QLatin1String("--any"), QLatin1String("a file with no tags matches no tag")},
    {QLatin1String("--since"), QLatin1String("an untagged file was never tagged")},
};

int cmdLs(const Globals &g, Session &s, const QStringList &ands, const QStringList &anys,
           bool untagged, bool cached, const QStringList &exts, qint64 sinceSeconds,
           const QStringList &positionals)
{
    // The contradictions before anything else is resolved: a flag that cannot
    // be combined is wrong whatever the paths turn out to be, so `ls --untagged
    // --any x /nope` names the flag rather than sending somebody after the
    // path.
    //
    // A file with no tags has no time at which it was tagged and no tag to
    // match, so these flags cannot narrow the answer -- only empty it, which
    // would make `ls --untagged --since 1h` look like a folder with nothing new
    // in it.
    if (untagged) {
        // Named for what was typed, so somebody who wrote `--any` is not told
        // about `--tag`.
        const QString typed = !ands.isEmpty()   ? QStringLiteral("--tag")
            : !anys.isEmpty()                   ? QStringLiteral("--any")
            : sinceSeconds >= 0                 ? QStringLiteral("--since")
                                                : QString();
        for (const Exclusion &excluded : kUntaggedExcludes) {
            if (typed != excluded.flag)
                continue;
            fail(QStringLiteral("ls: --untagged and %1 ask opposite questions; %2")
                     .arg(excluded.flag, excluded.why));
            return kUsage;
        }
    }
    // `--cached` is a way of answering `--untagged`, not a question of its own.
    // Every other listing already comes out of the index, which is one file and
    // is read whole; there is nothing a cache could make faster about it, so a
    // flag accepted there would be a flag that did nothing.
    if (cached && !untagged) {
        fail(QStringLiteral("ls: --cached answers --untagged without walking the disk; "
                            "every other listing is the index, which is already read "
                            "whole"));
        return kUsage;
    }

    int status = kOk;
    const QString dir = optionalDir(positionals, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QVector<Record> hits;
    int undated = 0;
    QStringList unreadable;
    QStringList unnamed;
    QStringList skipped;
    // Whole trees the cached answer could not cover. Kept apart from `skipped`
    // because these are sentences about a root and that one is suffixed with
    // `it was not listed`, which is about a path.
    QStringList uncovered;
    if (untagged && cached) {
        // The whole point is that nothing here touches the disk: no walk, and
        // no `stat` on the way out either. What comes back is what the tree
        // held when the list was built, which is why the age is said out loud
        // below and why plain `--untagged` stays exact.
        QVector<Root> scope;
        if (dir.isEmpty()) {
            scope = roots();
        } else if (const Root *holding = rootFor(dir)) {
            scope.append(*holding);
        }
        if (roots().isEmpty()) {
            fail(QStringLiteral("ls: --cached answers out of a root's list of names, "
                                "and no root is declared; `omafiles roots add` declares "
                                "the trees this machine keeps tags on"));
            return kUsage;
        }
        if (scope.isEmpty()) {
            fail(QStringLiteral("ls: %1 is under no declared root, so there is no list "
                                "of names covering it; plain `--untagged` walks it")
                     .arg(dir));
            return kUsage;
        }

        qint64 oldest = -1;
        for (const Root &root : std::as_const(scope)) {
            // A disk in a drawer has a list and the paths in it are not
            // reachable. Saying so is the same answer every other verb gives.
            if (!rootIsReachable(root)) {
                uncovered.append(QStringLiteral("root %1 is declared and not here, so "
                                                "nothing under it was listed")
                                     .arg(root.id));
                continue;
            }
            QStringList names;
            qint64 age = -1;
            QString error;
            if (!readNames(root, &names, &age, &error)) {
                uncovered.append(error);
                continue;
            }
            if (age < 0) {
                uncovered.append(QStringLiteral("root %1 has no list of names yet, so "
                                                "nothing under it was listed; "
                                                "`omafiles sweep` builds one")
                                     .arg(root.id));
                continue;
            }
            oldest = qMax(oldest, age);
            for (const QString &path : std::as_const(names)) {
                // `tagWalk` does not yield the directory it was pointed at, so
                // neither does this: `ls --untagged ~/docs` is about what is in
                // ~/docs.
                if (!dir.isEmpty() && (path == dir || !isUnderDirectory(path, dir)))
                    continue;
                if (!s.store.get(path).tags.isEmpty())
                    continue;
                if (!matchesExt(path, exts))
                    continue;
                Record row;
                row.path = path;
                hits.append(row);
            }
        }
        // Said whatever the age turns out to be, because the contract is that
        // this answer is as old as the last sweep and not that it is fresh. A
        // note beside a run that succeeded, so `--quiet` takes it away.
        if (oldest >= 0) {
            warn(g, QStringLiteral("ls: from a list of names built %1 ago; "
                                   "`omafiles sweep` rebuilds it, and plain --untagged "
                                   "walks the disk and is exact")
                        .arg(spelledDuration(oldest)));
        }
    } else if (untagged) {
        // `[dir]` is optional, and `verify`, `reindex` and `prune` all read an
        // absent one as the current directory.
        const QString root = dir.isEmpty() ? QDir::currentPath() : dir;
        // The same entries `verify` and `reindex` walk, so `--untagged` can
        // answer for a dotfile and for a directory -- and a directory nobody
        // may open is reported rather than passed over in silence.
        for (const QString &canonical : tagWalk(root, false, &unreadable, &unnamed)) {
            QString error;
            const Record record = s.store.get(canonical);
            if (!record.tags.isEmpty())
                continue;
            if (!matchesExt(canonical, exts))
                continue;
            Record stampedRecord = record;
            if (stampedRecord.path.isEmpty()) {
                stampedRecord.path = canonical;
                // The same three-way answer the indexed branch below gives. A
                // directory readable but not searchable lists its entries and
                // will not stat one of them, and dropping that entry in silence
                // is a short list reported as a whole one.
                const PathState state = pathState(canonical, &error);
                if (state == PathState::Unknown || state == PathState::Unreachable) {
                    skipped.append(error);
                    continue;
                }
                if (state == PathState::Missing)
                    continue;
            }
            hits.append(stampedRecord);
        }
    } else {
        if (cannotLookInto(dir))
            unreadable.append(dir);
        for (const Record &record : s.store.records()) {
            if (!dir.isEmpty() && !isUnderDirectory(record.path, dir))
                continue;
            if (!matchesTags(record, ands, anys))
                continue;
            if (!matchesExt(record.path, exts))
                continue;
            // Asked after the other filters for the same reason the count
            // below is: a row this query was never going to return is not a
            // row to say anything about, and an unplugged drive holding rows
            // in another tree would otherwise bury the answer in notes.
            QString error;
            const PathState state = pathState(record.path, &error);
            // "I may not look" is not "it is gone". The row is still skipped
            // -- nothing here can list what the system will not describe -- but
            // it is skipped out loud.
            if (state == PathState::Unknown || state == PathState::Unreachable) {
                skipped.append(error);
                continue;
            }
            if (state == PathState::Missing)
                continue;
            // Counted here, after every other filter, so the number is about
            // the rows this query would otherwise have returned rather than
            // every undated row in the index.
            if (sinceSeconds >= 0 && record.tagged <= 0) {
                ++undated;
                continue;
            }
            if (!matchesSince(record, sinceSeconds))
                continue;
            hits.append(record);
        }
    }

    std::sort(hits.begin(), hits.end(),
              [](const Record &a, const Record &b) { return a.path < b.path; });

    if (g.json) {
        QJsonArray array;
        for (const Record &record : hits)
            array.append(record.toJson());
        printJson(array);
    } else {
        for (const Record &record : hits)
            out() << record.path << '\n';
    }
    for (const QString &closed : unreadable) {
        warn(g, QStringLiteral("ls: %1 is unreadable; nothing under it was listed")
                    .arg(closed));
    }
    for (const QString &mangled : unnamed) {
        warn(g, QStringLiteral("ls: %1 cannot be named; the name is not valid UTF-8, "
                               "so neither it nor anything under it was listed")
                    .arg(mangled));
    }
    for (const QString &error : skipped) {
        // Not "the row": `--untagged` walks the disk, where a path this could
        // not look at has no row to be.
        warn(g, QStringLiteral("ls: %1; it was not listed").arg(error));
    }
    for (const QString &tree : uncovered)
        warn(g, QStringLiteral("ls: %1").arg(tree));
    if (undated > 0) {
        warn(g, QStringLiteral("ls: %1 matching row(s) predate the tagged field, so "
                               "--since cannot judge them; they are stamped the next "
                               "time their tags change")
                    .arg(undated));
    }
    // A short list is not the answer that was asked for, and a note on stderr
    // is not enough on its own, because `--quiet` is allowed to take it away.
    // The undated count is not one of these: those rows are there and the index
    // simply cannot date them, which is a narrower answer rather than a shorter
    // one. See `kIncomplete`.
    return unreadable.isEmpty() && unnamed.isEmpty() && skipped.isEmpty()
            && uncovered.isEmpty()
        ? kOk
        : kIncomplete;
}

// A verb that removes something asks first, unless told not to.
//
// Off a terminal it does not ask, it refuses: a script that would have been
// prompted is a script that never saw the question, and answering it by
// guessing is how a batch job deletes a directory nobody meant to name.
// How many of them the question names before it gives up and counts. Ten is
// what fits above a prompt on the shortest terminal anybody still uses: the
// question itself has to stay on the screen, because a question you have to
// scroll back to find is not one you answered. Past that the count above is the
// honest summary -- nobody audits a hundred paths at a prompt anyway.
constexpr int kNamedInPrompt = 10;

bool confirmed(const Globals &g, const QString &what, const QStringList &paths, int rows)
{
    if (g.yes)
        return true;
    const int count = paths.size();
    const QString scale = rows > count
        ? QStringLiteral("%1 path(s), %2 index row(s)").arg(count).arg(rows)
        : QStringLiteral("%1 path(s)").arg(count);
    // And it says which. A bare count is the same sentence whether the path is
    // the one you typed or the target a symlink led to. `rm -i` names the file
    // it is about to remove -- `remove regular file 'a.txt'?` -- for the same
    // reason.
    QString named;
    for (int i = 0; i < count && i < kNamedInPrompt; ++i)
        named += QStringLiteral("\n  %1").arg(paths.at(i));
    if (count > kNamedInPrompt)
        named += QStringLiteral("\n  ... and %1 more").arg(count - kNamedInPrompt);

    if (!::isatty(fileno(stdin))) {
        fail(QStringLiteral("%1: refusing %2 without a terminal to ask at; pass --yes%3")
                 .arg(what, scale, named));
        return false;
    }
    err() << what << ": " << scale << named << "\nContinue? [y/N] ";
    err().flush();
    QTextStream in(stdin);
    const QString answer = in.readLine().trimmed().toLower();
    return answer == QLatin1String("y") || answer == QLatin1String("yes");
}

// The vocabulary, and under `--tag`, the part of it that still divides a set.
//
// `tags --tag work` counts the tags that appear beside `work`, over the rows
// carrying it -- which is what the window walks when it reads tags as folders.
// The tags asked after are left out of the answer: they are the scope, not a
// division of it, and `work` is on every row by construction.
//
// A tag on every row of the scope is still printed. It divides nothing, so the
// window does not offer it as a folder, but that is a rule about what is worth
// walking into and this verb is asked what is there.
int cmdTags(const Globals &g, Session &s, const QStringList &scope,
            const QStringList &positionals)
{
    int status = kOk;
    const QString dir = optionalDir(positionals, &status);
    if (status != kOk)
        return status;
    if ((status = openSession(&s)) != kOk)
        return status;

    const bool scopeUnreadable = cannotLookInto(dir);
    QMap<QString, int> counts;
    QStringList skipped;
    for (const Record &record : s.store.records()) {
        if (!dir.isEmpty() && !isUnderDirectory(record.path, dir))
            continue;
        bool carries = true;
        for (const QString &tag : scope) {
            if (!record.tags.contains(tag)) {
                carries = false;
                break;
            }
        }
        if (!carries)
            continue;
        // The same question `ls` asks, and for the same reason: a tree nobody
        // may open is not a tree of deleted files, and this is the verb people
        // read to find out what tags they have. Scoped first, so the note is
        // about the tree that was asked after.
        QString error;
        const PathState state = pathState(record.path, &error);
        if (state == PathState::Unknown || state == PathState::Unreachable) {
            skipped.append(error);
            continue;
        }
        if (state == PathState::Missing)
            continue;
        for (const QString &tag : record.tags) {
            if (!scope.contains(tag))
                counts[tag] += 1;
        }
    }
    if (g.json) {
        QJsonArray array;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            array.append(QJsonObject{
                {QStringLiteral("tag"), it.key()},
                {QStringLiteral("count"), it.value()},
            });
        }
        printJson(array);
    } else {
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            out() << it.key() << '\t' << it.value() << '\n';
    }
    for (const QString &error : skipped) {
        warn(g, QStringLiteral("tags: %1; the row was not counted").arg(error));
    }
    if (scopeUnreadable) {
        warn(g, QStringLiteral("tags: %1 is unreadable; nothing under it was counted")
                    .arg(dir));
    }
    // A vocabulary short of the tags only those rows carry is not the
    // vocabulary. See `kIncomplete`.
    return scopeUnreadable || !skipped.isEmpty() ? kIncomplete : kOk;
}

// Drops rows whose file is gone, inside a tree -- or across the whole index,
// but only when asked in so many words. An absent directory means the current
// one, as it does for `verify` and `reindex`; `--all` is the whole index,
// spelled out, so the one destructive verb here is not the one with the widest
// reach by default.
int cmdPrune(const Globals &g, Session &s, bool all, const QStringList &positionals)
{
    int status = kOk;
    QString dir = optionalDir(positionals, &status);
    if (status != kOk)
        return status;
    if (all && !dir.isEmpty()) {
        fail(QStringLiteral("prune: --all is the whole index; it takes no directory"));
        return kUsage;
    }
    if (!all && dir.isEmpty())
        dir = QDir::currentPath();
    if ((status = openSession(&s)) != kOk)
        return status;

    if (!all && cannotLookInto(dir)) {
        warn(g, QStringLiteral("prune: %1 is unreadable; nothing under it was dropped")
                    .arg(dir));
        if (g.json) {
            printJson(QJsonObject{{QStringLiteral("dropped"), QJsonArray{}}});
        }
        return kIncomplete;
    }

    QStringList dropped;
    QStringList skipped;
    for (const Record &record : s.store.records()) {
        if (!all && !isUnderDirectory(record.path, dir))
            continue;
        QString error;
        const PathState state = pathState(record.path, &error);
        if (state == PathState::Present)
            continue;
        // "I may not look" is not "it is gone", and neither is "the disk is in
        // a drawer": a directory chmod'd shut reads as the first, and an
        // unmounted share or an unplugged drive would read as deletion and take
        // with it, on a filesystem that does not carry `user.*`, the only copy
        // of those tags on the machine. The second of the two is the whole
        // reason a root can be declared -- without one, an unmounted tree is
        // `ENOENT`, which is exactly what every file in it having been deleted
        // looks like. Skipping costs a stale row somebody can prune tomorrow;
        // guessing costs the tags.
        if (state == PathState::Unknown || state == PathState::Unreachable) {
            skipped.append(error);
            continue;
        }
        dropped.append(record.path);
    }

    // Destructive, so it asks like `rm` and `trash` do and refuses off a
    // terminal rather than guessing: a row whose file is not there right now is
    // not always a row for a file that is gone, and dropping it throws away
    // tags nothing else on the machine holds. Nothing has left memory yet.
    //
    // Named by scope, because a prompt saying only "3 path(s)" does not tell
    // you whether they are the three under your feet or three from a drive you
    // unplugged last year -- and past ten paths the list ends in `... and N
    // more` rather than naming them.
    const QString what = all ? QStringLiteral("prune --all (the whole index)")
                             : QStringLiteral("prune (under %1)").arg(dir);
    // Said before the question, not after the act: a row this run stepped over
    // is a row the person answering the prompt should know was not counted.
    for (const QString &error : skipped) {
        warn(g, QStringLiteral("prune: %1; the row was left alone").arg(error));
    }
    if (!dropped.isEmpty() && !g.dryRun
        && !confirmed(g, what, dropped, dropped.size()))
        return kUsage;
    for (const QString &path : dropped)
        s.store.remove(path);

    // A dropped row is a row nobody else will write again, so there is nothing
    // to hand to `finish`; inside a batch the removals simply wait, unspoken,
    // for the one commit at the end.
    if (s.deferred)
        return kOk;
    if (!g.dryRun) {
        QString error;
        if (!s.store.commit(&error)) {
            fail(error);
            return kStore;
        }
    }
    if (g.json) {
        QJsonArray array;
        for (const QString &path : dropped)
            array.append(path);
        printJson(QJsonObject{{QStringLiteral("dropped"), array}});
    } else {
        for (const QString &path : dropped)
            out() << path << '\n';
    }
    return kOk;
}

int cmdOpen(const Globals &g, const QStringList &paths)
{
    if (paths.isEmpty()) {
        fail(QStringLiteral("open: path required"));
        return kUsage;
    }
    // Every path planned before any of them is launched, the way `rm` and
    // `trash` resolve every operand up front: `open` has the least undo of the
    // three, because a launched program cannot be un-launched.
    //
    // Every path that is not there is named, as in `resolveAll`. A missing
    // opener is the same answer for every operand, so it is given once.
    QVector<OpenPlan> plans;
    int refusal = kOk;
    for (const QString &raw : paths) {
        OpenPlan plan;
        QString error;
        PathState state = PathState::Present;
        if (!planOpen(raw, &plan, &error, &state)) {
            fail(error);
            // The path resolved and the failure is the opener: one answer for
            // the whole list, so it is said once. Otherwise the statuses
            // `resolveAll` gives, and the rest of the list is still named.
            if (state == PathState::Present) {
                refusal = qMax(refusal, kUsage);
                break;
            }
            refusal = qMax(refusal, pathStatus(state));
            continue;
        }
        plans.append(plan);
    }
    if (refusal != kOk)
        return refusal;

    QJsonArray opened;
    // Past the gate, every one of them -- and a launch the desktop refused does
    // not cost the rest theirs, the way `rm` reports the operand it could not
    // remove and still removes the others.
    bool refused = false;
    for (int i = 0; i < paths.size(); ++i) {
        if (!g.dryRun) {
            QString error;
            if (!openLocalPath(paths.at(i), &error)) {
                fail(error);
                refused = true;
                continue;
            }
        } else if (!g.json) {
            out() << formatOpenPlan(plans.at(i)) << '\n';
        }
        if (g.json)
            opened.append(plans.at(i).toJson());
    }

    if (g.json)
        printJson(opened);
    return refused ? kUsage : kOk;
}


int cmdRecent(const Globals &g, Session &s, const QStringList &positionals)
{
    int limit = 20;
    if (positionals.size() > 1) {
        fail(QStringLiteral("recent: one count at most"));
        return kUsage;
    }
    if (!positionals.isEmpty()) {
        bool ok = false;
        limit = positionals.first().toInt(&ok);
        if (!ok || limit <= 0) {
            fail(QStringLiteral("recent: %1 is not a count").arg(positionals.first()));
            return kUsage;
        }
    }

    int status = openSession(&s);
    if (status != kOk)
        return status;

    QVector<Record> rows;
    QStringList skipped;
    int unknown = 0;
    for (const Record &record : s.store.records()) {
        // Counted after the row is known to be one this would have listed, as
        // in `ls`: index-wide, the count would report rows the query never had
        // anything to do with.
        //
        // And a path the system will not talk about is not a path that is gone.
        // A list of the newest things is read by what is at the top of it,
        // which is where a silently dropped row hides best.
        QString error;
        const PathState state = pathState(record.path, &error);
        if (state == PathState::Unknown || state == PathState::Unreachable) {
            skipped.append(error);
            continue;
        }
        if (state == PathState::Missing)
            continue;
        if (record.tagged <= 0) {
            ++unknown;
            continue;
        }
        rows.append(record);
    }
    std::sort(rows.begin(), rows.end(), [](const Record &a, const Record &b) {
        if (a.tagged != b.tagged)
            return a.tagged > b.tagged;
        return a.path < b.path;
    });
    if (rows.size() > limit)
        rows.resize(limit);

    if (unknown > 0) {
        warn(g, QStringLiteral("recent: %1 row(s) predate the tagged field and cannot be "
                               "ordered; they are stamped the next time their tags change")
                    .arg(unknown));
    }
    for (const QString &error : skipped) {
        warn(g, QStringLiteral("recent: %1; the row was not listed").arg(error));
    }

    if (g.json) {
        QJsonArray array;
        for (const Record &record : rows)
            array.append(record.toJson());
        printJson(array);
    } else {
        for (const Record &record : rows)
            out() << record.path << '\n';
    }
    // A list read by what is at the top of it, one row short and exiting 0.
    // The undated count is not one of these -- those rows are there and cannot
    // be ordered, which is a narrower answer rather than a shorter one. See
    // `kIncomplete`.
    return skipped.isEmpty() ? kOk : kIncomplete;
}

// ------------------------------------------------------------- completion

// What the shell should offer for the word being typed.
//
// This lives in the binary rather than in a generated shell script because the
// completion worth having here is the one no static script can produce: the
// tags after `--tag` are strings you invented, and they are in the index this
// process already knows how to open. A generated script would offer the verbs
// and stop exactly where the guessing starts.
//
// `words` is everything after the program name, with the partly typed word
// last. Returning nothing means "no opinion", and the stub falls back to the
// shell's own filename completion, which is right for every path argument.
QStringList completionFor(const QStringList &words)
{
    static const QStringList verbs = {
        QStringLiteral("tag"),    QStringLiteral("ls"),      QStringLiteral("tags"),
        QStringLiteral("has"),    QStringLiteral("json"),    QStringLiteral("recent"),
        QStringLiteral("open"),   QStringLiteral("mv"),      QStringLiteral("cp"),
        QStringLiteral("trash"),  QStringLiteral("rm"),      QStringLiteral("prune"),
        QStringLiteral("reindex"), QStringLiteral("sweep"),   QStringLiteral("verify"),
        QStringLiteral("export"),
        QStringLiteral("roots"),  QStringLiteral("view"),
        QStringLiteral("import"), QStringLiteral("batch"),   QStringLiteral("completions")};
    static const QStringList globals = {QStringLiteral("--json"), QStringLiteral("--dry-run"),
                                        QStringLiteral("--yes"), QStringLiteral("--quiet"),
                                        QStringLiteral("--help"), QStringLiteral("--version")};

    const QString current = words.isEmpty() ? QString() : words.last();
    const QStringList before = words.mid(0, qMax(0, words.size() - 1));
    const QString previous = before.isEmpty() ? QString() : before.last();

    // Tags, from the index, wherever a tag is what comes next. `tag merge` and
    // its alias take one as a bare argument rather than behind a flag, so both
    // `<from>` and `<to>` are offered from the vocabulary rather than falling
    // through to the shell's own file completion.
    bool mergingTags = false;
    for (int i = 0; i + 1 < before.size() && !mergingTags; ++i) {
        if (before.at(i) != QLatin1String("tag"))
            continue;
        const QString sub = before.at(i + 1);
        if (sub != QLatin1String("merge") && sub != QLatin1String("rename"))
            continue;
        mergingTags = before.size() - (i + 2) < 2;   // still on <from> or <to>
    }
    // A tag never starts with `-` (`normalizeTag` refuses one), so a word that
    // does is being typed as a flag and the vocabulary has no answer for it --
    // the same dead end as `tag -` below. The value after `--tag` is not: there
    // the word can only be a tag, which is where cobra stops offering flags too.
    if ((mergingTags && !current.startsWith(QLatin1Char('-')))
        || previous == QLatin1String("--tag") || previous == QLatin1String("-t")
        || previous == QLatin1String("--any")) {
        Store store(defaultIndexPath());
        if (!store.load(nullptr))
            return {};
        QStringList tags;
        for (const Record &record : store.records())
            tags.append(record.tags);
        return uniqueSorted(tags);
    }
    // The verb, and the word after it -- which is the subcommand where the verb
    // has any, and is read as one only for `tag`.
    QString verb;
    QString sub;
    for (const QString &word : before) {
        if (word.startsWith(QLatin1Char('-')))
            continue;
        if (verb.isEmpty()) {
            verb = word;
            continue;
        }
        sub = word;
        break;
    }

    // A word that begins with `-` is a request for flags, even where a
    // subcommand or a shell name would otherwise be offered: nothing in those
    // lists starts with `-`, so the reply would filter down to nothing while
    // exit 0 stopped the shell falling back to filenames. It is the shape
    // everyone else generates -- cargo's script tests `[[ "$cur" == -* ]]`
    // before the subcommand list, and cobra hands back flag names as soon as
    // `toComplete[0] == '-'`.
    if (current.startsWith(QLatin1Char('-')) && !verb.isEmpty()) {
        QStringList flags = globals;
        // Except the one global that one verb refuses: `--json` after
        // `completions` would be completion for an error message.
        if (verb == QLatin1String("completions"))
            flags.removeAll(QStringLiteral("--json"));
        // From the same table `dispatch` gates the parse on, because the six
        // `tag` subcommands do not share their flags: offering the family's
        // `--tag` and `--from` everywhere would suggest flags that `tag ls`,
        // `tag clear` and `tag merge` refuse.
        if (verb == QLatin1String("tag")) {
            const Takes takes = tagTakes(sub);
            if (takes.tag)
                flags << QStringLiteral("--tag");
            if (takes.from)
                flags << QStringLiteral("--from");
        }
        if (verb == QLatin1String("ls") || verb == QLatin1String("has"))
            flags << QStringLiteral("--tag");
        if (verb == QLatin1String("ls")) {
            flags << QStringLiteral("--any") << QStringLiteral("--untagged")
                  << QStringLiteral("--ext") << QStringLiteral("--since");
            // Not the ones already contradicted by what is on the line: a flag
            // the verb will refuse is completion for an error message. Asked of
            // the same table the refusal reads.
            // `--cached` is a way of answering `--untagged` and is refused
            // without it, so it is offered only once that is on the line.
            const bool untagged = before.contains(QLatin1String("--untagged"));
            if (untagged)
                flags << QStringLiteral("--cached");
            for (const Exclusion &excluded : kUntaggedExcludes) {
                if (untagged)
                    flags.removeAll(QString(excluded.flag));
                if (before.contains(excluded.flag)
                    || (excluded.flag == QLatin1String("--tag")
                        && before.contains(QLatin1String("-t"))))
                    flags.removeAll(QStringLiteral("--untagged"));
            }
        }
        if (verb == QLatin1String("reindex"))
            flags << QStringLiteral("--from-xattr");
        if (verb == QLatin1String("mv"))
            flags << QStringLiteral("--replace") << QStringLiteral("--with")
                  << QStringLiteral("--pattern");
        if (verb == QLatin1String("sweep"))
            flags << QStringLiteral("--status");
        if (verb == QLatin1String("prune"))
            flags << QStringLiteral("--all");
        return uniqueSorted(flags);
    }

    if (previous == QLatin1String("completions"))
        return {QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish")};

    if (verb == QLatin1String("tag") && sub.isEmpty()) {
        return {QStringLiteral("add"),   QStringLiteral("rm"),     QStringLiteral("ls"),
                QStringLiteral("clear"), QStringLiteral("cp"),     QStringLiteral("merge"),
                QStringLiteral("rename")};
    }

    if (verb.isEmpty())
        return verbs + globals;

    return {};                                   // a path: the shell knows better
}

int cmdComplete(const QStringList &words)
{
    const QStringList candidates = completionFor(words);
    if (candidates.isEmpty())
        return 1;                                // no opinion; let the shell try files
    for (const QString &candidate : candidates)
        out() << candidate << '\n';
    return kOk;
}

// The stub is ten lines because the thinking is in the binary. What ships in
// /usr/share is a shim that outlives every change to the command set, and a
// shell that has omafiles has everything the completion needs -- no second tool
// to install, and no completions that silently stop working when it is missing.
int cmdCompletions(const Globals &g, const QStringList &positionals)
{
    // A shell script is not a JSON document and there is no shape for it to
    // take. A global every verb takes except one has to be refused by that one,
    // or it is not an exception.
    if (g.jsonAsked) {
        fail(QStringLiteral("completions: --json has no meaning here; this prints a "
                            "shell script, which has no JSON shape"));
        return kUsage;
    }
    if (positionals.size() != 1) {
        fail(QStringLiteral("completions: one shell required (bash, zsh, fish)"));
        return kUsage;
    }
    const QString shell = positionals.first();
    if (shell == QLatin1String("bash")) {
        out() << R"SHELL(# omafiles completion for bash
_omafiles() {
    local IFS=$'\n' candidates
    if candidates=$(omafiles __complete -- "${COMP_WORDS[@]:1}" 2>/dev/null); then
        COMPREPLY=($(compgen -W "$candidates" -- "${COMP_WORDS[COMP_CWORD]}"))
    else
        COMPREPLY=($(compgen -f -- "${COMP_WORDS[COMP_CWORD]}"))
    fi
}
complete -o filenames -F _omafiles omafiles
)SHELL";
        return kOk;
    }
    if (shell == QLatin1String("zsh")) {
        out() << R"SHELL(#compdef omafiles
_omafiles() {
    local -a candidates
    # "${(@)...}" keeps the empty last word. Unquoted, zsh drops it, and the
    # binary then completes the previous word instead of the one being typed --
    # so `--tag <TAB>` offered flags where it should offer your tags.
    if candidates=(${(f)"$(omafiles __complete -- "${(@)words[2,-1]}" 2>/dev/null)"}) \
        && [[ -n "$candidates" ]]; then
        compadd -- $candidates
    else
        _files
    fi
}
compdef _omafiles omafiles
)SHELL";
        return kOk;
    }
    if (shell == QLatin1String("fish")) {
        out() << R"SHELL(# omafiles completion for fish
function __omafiles_complete
    # Quoted, so an empty token is still one argument. Bare, fish's command
    # substitution of an empty line yields nothing at all, and the binary
    # completes the previous word instead of the one being typed.
    set -l cur (commandline -ct)
    set -l words (commandline -opc)
    set -l offered (omafiles __complete -- $words[2..-1] "$cur" 2>/dev/null)
    if test $status -eq 0
        printf '%s\n' $offered
    else
        # Only where the binary has no opinion, which is where a path goes.
        # Registering this as a second source offered filenames everywhere,
        # ahead of the verbs.
        __fish_complete_path $cur
    end
end
complete -c omafiles -f -a '(__omafiles_complete)'
)SHELL";
        return kOk;
    }
    fail(QStringLiteral("completions: unknown shell %1 (bash, zsh, fish)").arg(shell));
    return kUsage;
}

// ---------------------------------------------------------------- destructive

// Commits here rather than through `finish`.
//
// It has already printed the rows it dropped, and `commitStore` prints the rows
// it wrote -- routing this through it would answer one question twice. The only
// two verbs that reach here are `rm` and `trash`, and `batchTakes` refuses both
// on the grounds that a rolled back index cannot un-delete a file, so this can
// never be the commit a deferred session was saving up.
int dropRows(const Globals &g, Store *store, const QStringList &paths, QJsonArray *done)
{
    for (const QString &path : paths) {
        store->remove(path);
        if (g.json)
            done->append(path);
        else
            out() << path << '\n';
    }
    if (g.dryRun)
        return kOk;
    QString error;
    if (!store->commit(&error)) {
        fail(error);
        return kStore;
    }
    return kOk;
}

// -------------------------------------------------------------------- verbs

int cmdTagClear(const Globals &g, Session &s, const QStringList &paths)
{
    if (paths.isEmpty()) {
        fail(QStringLiteral("tag clear: path required"));
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QString error;
    QVector<Record> written;
    // The paths the index says nothing about, so the run can tell whether a
    // refused removal below left a drop nothing recorded.
    for (const QString &path : resolved) {
        const Record existing = s.store.get(path);
        const bool rowless = existing.path.isEmpty();
        // Dropping no tag is not a tagging event, so a path with nothing to
        // drop keeps the stamp it had -- which for a path with no row is none
        // at all. Same rule as `tag add` and `tag rm`: the wall clock would
        // date a change that never happened, which is what `--json` prints.
        const qint64 carried = existing.tags.isEmpty() ? keptStamp(existing) : 0;
        PathState state = PathState::Missing;
        const Record record = stamped(path, {}, &error, &state, carried);
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        // Same as `tag rm`: no row means no receipt, even when the tags list
        // is empty.
        if (!rowless)
            written.append(record);
        // A drop the filesystem refuses on a path with no row is a drop nothing
        // recorded: the value is still on the file, `verify` goes on reporting
        // it, and this is the command it names. Exit 0 there would claim a
        // removal that did not happen.
        //
        // Only there. Where a row existed, clearing it is the record -- the
        // same reason `tag add` exits 0 over a refused mirror, and what keeps a
        // filesystem with no `user.*` namespace from failing every `tag clear`.
        mirrorTags(g, s, record, rowless);
    }
    if ((status = finish(g, s, written)) != kOk)
        return status;
    // Nothing to say from inside a batch, which reads `s.incomplete` after its
    // own commit: the flush the count comes from has not run yet.
    return s.incomplete ? kIncomplete : kOk;
}

int cmdTagCp(const Globals &g, Session &s, const QString &from, const QStringList &paths)
{
    if (from.isEmpty()) {
        fail(QStringLiteral("tag cp: --from is required"));
        return kUsage;
    }
    if (paths.isEmpty()) {
        fail(QStringLiteral("tag cp: path required"));
        return kUsage;
    }
    int status = kOk;
    const QStringList source = resolveExisting({from}, &status);
    if (status != kOk)
        return status;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    const QStringList tags = s.store.get(source.first()).tags;
    // Nothing to copy is a mistake, not an instruction: copying from a file
    // with no tags cannot mean "clear the destination", so it is refused.
    if (tags.isEmpty()) {
        fail(QStringLiteral("tag cp: %1 has no tags to copy").arg(source.first()));
        return kUsage;
    }

    QString error;
    QVector<Record> written;
    for (const QString &path : resolved) {
        if (path == source.first())
            continue;
        // Added to what is already there, not written over it: `cp` names the
        // direction the tags travel, not a promise to make the two files
        // identical.
        const Record before = s.store.get(path);
        QStringList next = before.tags;
        next.append(tags);
        // Same rule as `tag add`: a write that adds no tag is not a tagging
        // event, so the row keeps the stamp it had rather than climbing to the
        // top of `recent` for having been asked a second time.
        const qint64 carried = uniqueSorted(next) == before.tags ? keptStamp(before) : 0;
        PathState state = PathState::Missing;
        const Record record = stamped(path, next, &error, &state, carried);
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        mirrorTags(g, s, record);
    }
    return finish(g, s, written);
}

int cmdTagMerge(const Globals &g, Session &s, const QStringList &positionals)
{
    if (positionals.size() != 2) {
        fail(QStringLiteral("tag merge: <from> <to> required"));
        return kUsage;
    }
    QString error;
    const QString from = normalizeTag(positionals.at(0), &error);
    if (from.isEmpty()) {
        fail(error);
        return kUsage;
    }
    const QString to = normalizeTag(positionals.at(1), &error);
    if (to.isEmpty()) {
        fail(error);
        return kUsage;
    }

    int status = openSession(&s);
    if (status != kOk)
        return status;

    QVector<Record> written;
    for (Record record : s.store.records()) {
        if (!record.tags.contains(from))
            continue;
        QStringList next;
        for (const QString &tag : record.tags)
            next.append(tag == from ? to : tag);
        record.tags = uniqueSorted(next);
        s.store.put(record);
        written.append(record);
        // Present only, and deliberately: this is not "is the row still true",
        // it is "is there a file here to stamp". Gone and unanswerable both
        // mean there is nothing this run can write -- a `setxattr` on either
        // fails -- and attempting it would only turn a row about an unplugged
        // drive into an error on a run that did everything it could. Nothing is
        // hidden by stepping over it: `verify` reports that path as
        // `unreadable` whether or not this verb ever touched it.
        if (statable(record.path, nullptr))
            mirrorTags(g, s, record);
    }
    return finish(g, s, written);
}

// Exit 1 means "no", so `omafiles has … && something` reads as a sentence.
//
// It is the same 1 this program's table gives a usage error or a refusal, so
// unlike `grep -q` -- which keeps 2 for the error and 1 for the miss -- the exit
// code alone does not separate the two. Two other things do: stderr carries a
// message only when it went wrong, and `--json` prints an object only when
// there was an answer to print.
int cmdHas(const Globals &g, Session &s, const QStringList &rawTags, const QStringList &paths)
{
    if (rawTags.size() != 1) {
        fail(QStringLiteral("has: one --tag required"));
        return kUsage;
    }
    if (paths.size() != 1) {
        fail(QStringLiteral("has: one path required"));
        return kUsage;
    }
    QString error;
    const QString tag = normalizeTag(rawTags.first(), &error);
    if (tag.isEmpty()) {
        fail(error);
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;
    if ((status = openSession(&s)) != kOk)
        return status;

    const bool held = s.store.get(resolved.first()).tags.contains(tag);
    // The exit code carries the answer; this makes it readable to a JSON
    // consumer too, which would otherwise get an empty stream.
    if (g.json) {
        printJson(QJsonObject{{QStringLiteral("path"), resolved.first()},
                              {QStringLiteral("tag"), tag},
                              {QStringLiteral("has"), held}});
    }
    return held ? kOk : 1;
}

int cmdJson(const Globals &, Session &s, const QStringList &paths)
{
    if (paths.size() != 1) {
        fail(QStringLiteral("json: one path required"));
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(paths, &status);
    if (status != kOk)
        return status;
    if ((status = openSession(&s)) != kOk)
        return status;
    Record record = s.store.get(resolved.first());
    if (record.path.isEmpty())
        return kOk;
    printJson(record.toJson());
    return kOk;
}

// The index as it is stored, one row per line -- or as one document, when the
// caller says it wants a document. jsonl is many documents and a JSON parser
// stops at the second line, so `--json` gives an array; the default hands back
// the file's own shape, which is the point of `export`.
int cmdExport(const Globals &g, Session &s)
{
    const int status = openSession(&s);
    if (status != kOk)
        return status;
    if (g.json) {
        QJsonArray rows;
        for (const Record &record : s.store.records())
            rows.append(record.toJson());
        printJson(rows);
        return kOk;
    }
    for (const Record &record : s.store.records()) {
        out() << QString::fromUtf8(
            QJsonDocument(record.toJson()).toJson(QJsonDocument::Compact)) << '\n';
    }
    return kOk;
}

// One document out of a dump, and what a diagnostic calls it. A jsonl dump has
// lines and an array has rows; calling row 5 of a one-line array "line 5" would
// send the reader to the wrong place.
struct DumpEntry {
    QString label;
    QJsonObject row;
};

// Every document in a dump, in either shape `export` writes one.
//
// jsonl is the default and the interchange form -- many documents, one row per
// line, appendable and readable a line at a time. `--json` writes the same rows
// as one array, because a JSON parser stops at the second document, and
// `--json` is a global with an `OMAFILES_JSON` behind it: in a shell that
// exports one, the `export | import -` this verb advertises is an array on both
// ends. A shape `export` writes is a dump, and every dump comes home.
bool readDump(const QByteArray &blob, QVector<DumpEntry> *entries, QString *error)
{
    const QByteArray whole = blob.trimmed();
    if (whole.startsWith('[')) {
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(whole, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isArray()) {
            *error = QStringLiteral("import: the dump is not a JSON array: %1")
                         .arg(parse.errorString());
            return false;
        }
        const QJsonArray rows = doc.array();
        for (int i = 0; i < rows.size(); ++i) {
            if (!rows.at(i).isObject()) {
                *error = QStringLiteral("import: row %1 is not a JSON object").arg(i + 1);
                return false;
            }
            entries->append({QStringLiteral("row %1").arg(i + 1), rows.at(i).toObject()});
        }
        return true;
    }
    int number = 0;
    for (const QByteArray &raw : blob.split('\n')) {
        ++number;
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            *error = QStringLiteral("import: line %1 is not a JSON object").arg(number);
            return false;
        }
        entries->append({QStringLiteral("line %1").arg(number), doc.object()});
    }
    return true;
}

// The file a verb reads its input out of, or stdin where `-` names it.
//
// `-` is stdin the way it is for every tool that reads a stream -- `git apply
// -`, `kubectl apply -f -`, `sort -` -- and a file really called `-` is still
// reachable as `./-`.
//
// Returns kOk, or what the failure costs: a name that is not there is 2, and a
// file that is there and will not open is a path nobody may look at, which is
// the 1 every other verb gives for one. See `pathStatus`.
int openInput(const QString &verb, const QString &source, QFile *file)
{
    if (source == QLatin1String("-")) {
        if (file->open(stdin, QIODevice::ReadOnly | QIODevice::Text))
            return kOk;
    } else {
        file->setFileName(source);
        if (file->open(QIODevice::ReadOnly | QIODevice::Text))
            return kOk;
    }
    fail(QStringLiteral("%1: cannot read %2").arg(verb, source));
    return pathStatus(pathState(source, nullptr));
}

// A merge, not a replacement: an import that silently dropped the rows it did
// not mention would make restoring one machine's tags onto another a way of
// losing the second machine's.
int cmdImport(const Globals &g, Session &s, const QStringList &positionals)
{
    if (positionals.size() != 1) {
        fail(QStringLiteral("import: one file required"));
        return kUsage;
    }
    // So `omafiles export | ssh box omafiles import -` works.
    const QString source = positionals.first();
    QFile file;
    int status = openInput(QStringLiteral("import"), source, &file);
    if (status != kOk)
        return status;

    status = openSession(&s);
    if (status != kOk)
        return status;

    QVector<DumpEntry> entries;
    QString why;
    // Bad input, not a broken store: exit 3 is what a caller reads as "the
    // index could not be written".
    if (!readDump(file.readAll(), &entries, &why)) {
        fail(why);
        return kUsage;
    }

    QVector<Record> written;
    for (const DumpEntry &entry : entries) {
        QString error;
        Record record = Record::fromJson(entry.row, &error);
        if (record.path.isEmpty()) {
            fail(QStringLiteral("import: %1: %2").arg(entry.label, error));
            return kUsage;
        }
        // Canonical, like every other write: a dump naming a file through a
        // symlinked parent would otherwise get a second row for a file that
        // already has one, which `tags` counts twice and `tag ls` can only find
        // under one of the two names. A row that never resolves is left alone
        // -- an import of a dump from another machine is a real use, and
        // refusing it because the file is not here would answer a different
        // question.
        const QString resolved = canonicalPath(record.path, nullptr);
        if (!resolved.isEmpty()) {
            record.path = resolved;
        } else if (!QDir::isAbsolutePath(record.path)) {
            // A row nothing can reach again. Every other verb resolves what it
            // is given, so a relative path here would make a row that `export`
            // can show and nothing else can touch -- not `tag ls`, not `ls`,
            // and not `prune`, which scopes on absolute prefixes. A row for a
            // file simply not on this machine is still fine: a dump from
            // somewhere else is a real thing to merge.
            fail(QStringLiteral("import: %1: %2 is not an absolute path")
                     .arg(entry.label, record.path));
            return kUsage;
        }
        // Validated like every other write. Unchecked, an import could put
        // `Has Space!` or `UPPER` into the index -- tags `tag rm` can never
        // match, because it normalises before comparing -- and a tag holding a
        // comma would come back from `reindex --from-xattr` as two, that being
        // the xattr's separator.
        QString tagError;
        const QStringList clean = normalizeTags(record.tags, &tagError);
        if (clean.isEmpty() && !record.tags.isEmpty()) {
            fail(QStringLiteral("import: %1: %2").arg(entry.label, tagError));
            return kUsage;
        }
        record.tags = clean;

        const Record existing = s.store.get(record.path);
        QStringList merged = existing.tags;
        merged.append(record.tags);
        record.tags = uniqueSorted(merged);
        // A merge adds; it does not un-know things. A dump line with no
        // `tagged` field must not take the stamp off the row it merges into, or
        // `--since` can never judge that file again.
        record.tagged = qMax(record.tagged, existing.tagged);
        s.store.put(record);
        written.append(record);
        // Present only, the same deliberate answer `tag merge` gives: the
        // question is whether there is a file here to stamp, not whether the
        // row is true. A dump from another machine names paths that are not on
        // this one at all -- that is the point of merging one -- and a row the
        // system will not talk about is no more writable than a row for a file
        // that was never here.
        if (statable(record.path, nullptr))
            mirrorTags(g, s, record);
    }
    return finish(g, s, committedRows(s.store, written));
}

// Every row for something inside `root`, including the row for `root` itself.
//
// A path is not a prefix of another just because its text is: `/a/dir` must not
// catch `/a/dirty.txt`, so the comparison is against the path plus a separator.
QStringList rowsUnder(const Store &store, const QString &root)
{
    const QString inside = root + QLatin1Char('/');
    QStringList paths;
    for (const Record &record : store.records()) {
        if (record.path == root || record.path.startsWith(inside))
            paths.append(record.path);
    }
    return paths;
}

// Whether there is already something at that path, symlinks included.
//
// `QFileInfo::exists` follows the link, so a dangling one would read as free
// space and walk straight through the clobber guard. A link is a thing at that
// path whether or not what it points at is; coreutils will not write through a
// dangling one either -- `cp` says "not writing through dangling symlink" in so
// many words.
bool occupied(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

// Why the destination's parent cannot hold a file, or empty if it can.
//
// The system's own reason where it has one: `QFileInfo::exists` is false for a
// directory nobody may look at as well as for one that is not there, so a bare
// `strerror(ENOENT)` about a chmod'd parent sends the reader after the wrong
// thing. The reason names the parent, because the destination beside it does
// not say which of its components is the one that will not answer.
QString unusableParent(const QString &parent)
{
    QString detail;
    const PathState state = pathState(parent, &detail);
    if (state == PathState::Unknown)
        return detail;
    if (state == PathState::Missing)
        return QString::fromLocal8Bit(strerror(ENOENT));
    return QFileInfo(parent).isDir() ? QString()
                                     : QString::fromLocal8Bit(strerror(ENOTDIR));
}

// Whether two paths sit on the same filesystem.
//
// `st_dev` is what `rename(2)` compares before it answers EXDEV, and what
// coreutils `mv` asks before it decides whether a rename is enough. Asked of a
// path that cannot be stat'd it says yes, so a dry run never invents a refusal
// it cannot justify -- the real run is there to give the system's own reason.
bool sameDevice(const QString &one, const QString &other)
{
    struct stat first {};
    struct stat second {};
    if (::stat(QFile::encodeName(one).constData(), &first) != 0
        || ::stat(QFile::encodeName(other).constData(), &second) != 0)
        return true;
    return first.st_dev == second.st_dev;
}

// The bytes a symlink holds, exactly as they were written, or empty on failure.
//
// Not `QFile::symLinkTarget`, which resolves a relative target against the
// link's own directory: `../t.txt` would come back a different link. No link
// holds an empty target, so empty is unambiguous.
QByteArray linkTarget(const QByteArray &path)
{
    // Grown rather than sized from `st_size`, which a concurrent rewrite of the
    // link would make a silent truncation.
    for (int size = 256;; size *= 2) {
        QByteArray buffer(size, '\0');
        const ssize_t read = ::readlink(path.constData(), buffer.data(), size);
        if (read < 0)
            return {};
        if (read < size) {
            buffer.resize(read);
            return buffer;
        }
    }
}

// Moves the path as named, link and all. See `resolveNamed`.
//
// `rename(2)` is the whole move within one filesystem; across one it answers
// EXDEV. A symlink then crosses as `readlink` + `symlink` + `unlink`, because
// `QFile::copy` would read through the link. A directory never reaches here;
// `cmdMove` refuses it.
//
// `copied` is the one of those three that leaves the file's own copy of its
// tags behind, and it is set here because this is where the choice is made.
// See `followMove`.
bool moveNamed(const QString &from, const QString &to, QString *error, bool *copied)
{
    const QByteArray source = QFile::encodeName(from);
    const QByteArray destination = QFile::encodeName(to);
    *copied = false;
    if (::rename(source.constData(), destination.constData()) == 0)
        return true;
    // Kept before building any message can overwrite it: the reason is the
    // system's to give.
    int failure = errno;
    if (failure != EXDEV) {
        if (error)
            *error = QString::fromLocal8Bit(strerror(failure));
        return false;
    }
    if (!QFileInfo(from).isSymLink()) {
        // `QFile::rename` carries the copy-and-delete fallback.
        QFile file(from);
        if (file.rename(to)) {
            *copied = true;
            return true;
        }
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray target = linkTarget(source);
    if (!target.isEmpty()
        && ::symlink(target.constData(), destination.constData()) == 0) {
        if (::unlink(source.constData()) == 0)
            return true;
        // The link is at both ends and the source will not go: take the new
        // one back rather than leave the move half done.
        failure = errno;
        ::unlink(destination.constData());
    } else {
        failure = errno;
    }
    if (error)
        *error = QString::fromLocal8Bit(strerror(failure));
    return false;
}

// Where a row filed under `was` lands once `from` has become `to`.
QString movedPath(const QString &was, const QString &from, const QString &to)
{
    return was == from ? to : to + QLatin1Char('/') + was.mid(from.size() + 1);
}

// The one shape `mv` and `cp` answer in, in both modes: where the file went is
// the thing every consumer needs, so it is always there. `rm` and `trash` say
// `{"removed":[...]}` the same way.
QJsonObject movedJson(const QString &from, const QString &to, const QVector<Record> &rows)
{
    QJsonArray array;
    for (const Record &record : rows)
        array.append(record.toJson());
    return QJsonObject{{QStringLiteral("from"), from},
                       {QStringLiteral("to"), to},
                       {QStringLiteral("rows"), array}};
}

// Follows a move with the index: every row under the folder, not just the row
// for the folder itself.
//
// The row that lands is the row that was there under its new name: the rename
// carried the file, so the tags and the stamp are already the answer -- a move
// is not a tagging event -- and the path is a string rewrite. Nothing here asks
// the system anything, which is why every row follows even through a directory
// nobody may look inside, and why the dry run in `cmdMove` builds the same rows
// the same way.
//
// `copied` says whether the file's own copy has to be written again. A rename
// keeps the inode, so the xattr is already there under the new name and a
// second write could only fail -- on a file nobody may write it would report a
// mirror that is already right. A crossing copy is the one that leaves it
// behind. A symlink holds no copy of its own either way: `setxattr` follows the
// link, so the tags sit on what it points at, and that does not move.
// `moveNamed` is where the three part company, so it says which one ran.
void followMove(const Globals &g, Session &s, const QString &from, const QString &to,
                QVector<Record> *written, bool copied)
{
    for (const QString &was : rowsUnder(s.store, from)) {
        Record record = s.store.get(was);
        record.path = movedPath(was, from, to);
        s.store.remove(was);
        s.store.put(record);
        written->append(record);
        if (copied)
            mirrorTags(g, s, record);
    }
}

int cmdMove(const Globals &g, Session &s, const QStringList &positionals)
{
    if (positionals.size() != 2) {
        fail(QStringLiteral("mv: takes exactly two paths, <old> and <new>"));
        return kUsage;
    }
    int status = kOk;
    // The path that was named, not the one it points at: a rename moves the
    // link. See `resolveNamed`. The destination keeps the link's own name for
    // the same reason -- `mv link.txt dest/` lands `dest/link.txt`.
    const QStringList source = resolveNamed({positionals.at(0)}, &status);
    if (status != kOk)
        return status;
    const QString from = source.first();
    const QFileInfo target(positionals.at(1));
    QString to = target.isDir()
        ? QDir(target.absoluteFilePath()).filePath(QFileInfo(from).fileName())
        : target.absoluteFilePath();

    // Resolved through its parent, because the destination does not exist yet:
    // a canonical source compared against an unresolved destination lets a
    // symlink walk straight past the inside-itself guard below.
    const QFileInfo parent(QFileInfo(to).absolutePath());
    if (parent.exists()) {
        const QString resolved = parent.canonicalFilePath();
        if (!resolved.isEmpty())
            to = resolved + QLatin1Char('/') + QFileInfo(to).fileName();
    }

    // A directory that is not there cannot hold the destination, and a `stat`
    // settles it. Made in both modes, like the already-exists and
    // cross-filesystem refusals: it is a fact about the path, there to be read,
    // with nothing about it waiting on the work.
    const QString unusable = unusableParent(parent.absoluteFilePath());
    if (!unusable.isEmpty()) {
        fail(QStringLiteral("mv: cannot move %1 to %2: %3").arg(from, to, unusable));
        return kUsage;
    }

    if (occupied(to)) {
        fail(QStringLiteral("mv: %1 already exists").arg(to));
        return kUsage;
    }

    if ((status = openSession(&s)) != kOk)
        return status;
    // A folder cannot be moved inside itself, and the rename would half succeed
    // before anyone noticed. Asked of the tree the source really is as well as
    // of the name it was given: `mv link link/inner` walks into the directory
    // at the far end of the link, which the link's own path does not show.
    const QString reached = canonicalPath(from, nullptr);
    if (isUnderDirectory(to, from)
        || (!reached.isEmpty() && isUnderDirectory(to, reached))) {
        fail(QStringLiteral("mv: %1 is inside %2").arg(to, from));
        return kUsage;
    }
    // A directory does not cross a filesystem, and a dry run has to say so.
    //
    // `rename(2)` answers EXDEV across devices and `QFile::rename`'s fallback
    // copies regular files only, so a file crosses and a directory does not.
    // The source's device against the destination's parent is the comparison
    // `rename` makes, and the parent because the destination does not exist
    // yet.
    //
    // The name and not what it points at, since `QFileInfo::isDir` follows the
    // link: a symlink is a link whatever it points at, and a link crosses.
    const QFileInfo named(from);
    if (named.isDir() && !named.isSymLink()
        && !sameDevice(from, QFileInfo(to).absolutePath())) {
        fail(QStringLiteral("mv: cannot move %1 to %2: %3; a folder cannot cross "
                            "filesystems -- move it another way, then `omafiles "
                            "reindex` and `omafiles prune`")
                 .arg(from, to, QString::fromLocal8Bit(strerror(EXDEV))));
        return kUsage;
    }

    if (g.dryRun) {
        // In the shape asked for: `--json --dry-run` must not hand a JSON
        // consumer an arrow between two paths.
        if (g.json) {
            // The rows the real run would rewrite, named where they would
            // land, built exactly the way `followMove` builds them: a move
            // carries the tags and the stamp across untouched, so the row is
            // already the answer and the preview cannot promise a row the real
            // run then fails to deliver.
            QVector<Record> plan;
            for (const QString &was : rowsUnder(s.store, from)) {
                Record record = s.store.get(was);
                record.path = movedPath(was, from, to);
                plan.append(record);
            }
            printJson(movedJson(from, to, plan));
        } else {
            // The destination, which is what the real run prints: a preview
            // whose shape differs from the result is a preview nothing can be
            // tested against.
            out() << to << '\n';
        }
        return kOk;
    }
    QString refused;
    bool copied = false;
    if (!moveNamed(from, to, &refused, &copied)) {
        // The reason is the system's to give: one guess for every failure
        // sends the reader after the wrong thing.
        fail(QStringLiteral("mv: cannot move %1 to %2: %3").arg(from, to, refused));
        return kUsage;
    }

    QVector<Record> written;
    followMove(g, s, from, to, &written, copied);

    if (!g.json)
        out() << to << '\n';
    // Announced here and not by the commit, because this verb has its own shape
    // to print and two documents on one stdout is not an answer. Printed after
    // the commit for the reason `rm` is: a destination reported before the row
    // that records it has landed is a claim the next run may contradict.
    const int done = finish(g, s, written, false);
    if (done != kOk)
        return done;
    if (g.json)
        printJson(movedJson(from, to, written));
    // The rename happened and every row followed it, so there is nothing left
    // for this to be short of. A file's own copy the filesystem refused is
    // `tag add`'s rule and not this one: named on stderr, exit 0, because the
    // row is the record.
    return kOk;
}

int cmdCopy(const Globals &g, Session &s, const QStringList &positionals)
{
    if (positionals.size() != 2) {
        fail(QStringLiteral("cp: takes exactly two paths, <old> and <new>"));
        return kUsage;
    }
    int status = kOk;
    // The path that was named. `cp` reads through a link -- POSIX says an
    // unadorned `cp` follows the source, and coreutils writes a plain file with
    // the target's bytes -- but it files the copy under the name it was handed:
    // `cp link.txt dest/` lands `dest/link.txt`, not the target's name.
    const QStringList source = resolveNamed({positionals.at(0)}, &status);
    if (status != kOk)
        return status;
    const QString from = source.first();

    // Named as a link, read through it. `resolveNamed` files the copy under
    // the name it was handed and is happy with a link that points nowhere,
    // because for `rm`, `trash` and `mv` a dangling link is a path that is
    // there -- but `cp` reads the bytes at the far end, and there are none.
    // That is the target missing and exit 2, as it is for every verb that
    // reads through a link.
    QString unreachable;
    PathState pointsAt = PathState::Present;
    if (canonicalPath(from, &unreachable, &pointsAt).isEmpty()) {
        fail(unreachable);
        return pathStatus(pointsAt);
    }

    const QFileInfo target(positionals.at(1));
    QString to = target.isDir()
        ? QDir(target.absoluteFilePath()).filePath(QFileInfo(from).fileName())
        : target.absoluteFilePath();

    // Resolved through its parent, because the destination does not exist yet:
    // a canonical source compared against an unresolved destination lets a
    // symlink walk straight past the inside-itself guard below.
    const QFileInfo parent(QFileInfo(to).absolutePath());
    if (parent.exists()) {
        const QString resolved = parent.canonicalFilePath();
        if (!resolved.isEmpty())
            to = resolved + QLatin1Char('/') + QFileInfo(to).fileName();
    }

    // A directory that is not there cannot hold the destination, and a `stat`
    // settles it. Made in both modes, like the already-exists refusal: it is a
    // fact about the path, there to be read, with nothing about it waiting on
    // the work.
    const QString unusable = unusableParent(parent.absoluteFilePath());
    if (!unusable.isEmpty()) {
        fail(QStringLiteral("cp: cannot copy %1 to %2: %3").arg(from, to, unusable));
        return kUsage;
    }

    if (occupied(to)) {
        fail(QStringLiteral("cp: %1 already exists").arg(to));
        return kUsage;
    }

    if ((status = openSession(&s)) != kOk)
        return status;
    // Looked up through the link, because the bytes come through the link: the
    // tags that travel with a copy are the tags of the file that was copied,
    // and rows are only ever filed under a fully resolved path.
    const QString reached = canonicalPath(from, nullptr);
    const QStringList tags = s.store.get(reached.isEmpty() ? from : reached).tags;

    // Before the dry run prints a copy, not in the failure branch of
    // `QFile::copy`, which a dry run never reaches. Nothing about a directory
    // changes between the two runs, so there is nothing for the dry run to find
    // out by doing the work.
    if (QFileInfo(from).isDir()) {
        fail(QStringLiteral("cp: %1 is a directory; cp takes files").arg(from));
        return kUsage;
    }

    if (g.dryRun) {
        // In the shape asked for: `--json --dry-run` must not hand a JSON
        // consumer an arrow between two paths.
        if (g.json) {
            // The row the real run would write. Built by hand rather than by
            // `stamped`, which refuses a path it cannot stat, and nothing has
            // been copied yet.
            QVector<Record> plan;
            if (!tags.isEmpty()) {
                Record record;
                record.path = to;
                record.tags = tags;
                record.tagged = QDateTime::currentSecsSinceEpoch();
                plan.append(record);
            }
            printJson(movedJson(from, to, plan));
        } else {
            // The destination, which is what the real run prints: a preview
            // whose shape differs from the result is a preview nothing can be
            // tested against.
            out() << to << '\n';
        }
        return kOk;
    }
    if (!QFile::copy(from, to)) {
        fail(QFileInfo(from).isDir()
                 ? QStringLiteral("cp: %1 is a directory; cp takes files").arg(from)
                 : QStringLiteral("cp: cannot copy %1 to %2: %3")
                       .arg(from, to, QString::fromLocal8Bit(strerror(errno))));
        return kUsage;
    }

    QVector<Record> written;
    if (!tags.isEmpty()) {
        QString error;
        PathState state = PathState::Missing;
        const Record record = stamped(to, tags, &error, &state);
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        // Through the Session like every other write, which is what emits the
        // JSON.
        mirrorTags(g, s, record);
    }
    // The destination is the answer, and it is not printed when the answer
    // is asked for as JSON: two things on stdout is not a document.
    if (!g.json)
        out() << to << '\n';
    // The commit stays quiet; this verb prints its own shape, after the row it
    // reports is certainly on disk. See `cmdMove`.
    const int done = finish(g, s, written, false);
    if (done != kOk)
        return done;
    if (g.json)
        printJson(movedJson(from, to, written));
    return kOk;
}

int cmdRemove(const Globals &g, Session &s, const QStringList &paths, bool toTrash)
{
    const QString verb = toTrash ? QStringLiteral("trash") : QStringLiteral("rm");
    if (paths.isEmpty()) {
        fail(QStringLiteral("%1: path required").arg(verb));
        return kUsage;
    }
    int status = kOk;
    // The path that was named, not the one it points at. See `resolveNamed`.
    // The refusal waits until every check below has spoken, so a mixed list
    // names each operand for its own reason.
    const QStringList resolved = resolveNamed(paths, &status);

    // Every refusal this verb can settle by looking, settled before it asks and
    // before it acts.
    //
    // Whether a name is a directory and whether `gio` is on PATH are both
    // there to be read, so neither waits on any work. A prompt that names three
    // paths for a run that will refuse the second, or a dry run that names tags
    // as about to go for a command that will not go, is worse than none.
    //
    // Every offending operand, not the first: coreutils `rm` and `chmod` report
    // each one they cannot handle rather than stopping at one, and a person
    // retyping the command wants the whole list.
    if (toTrash && QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty()) {
        fail(QStringLiteral("trash: gio is not installed; it comes with glib2 and "
                            "is what puts a file in the desktop's trash"));
        // Said once, and it does not take the more specific status off a path
        // that is not there.
        if (status == kOk)
            status = kUsage;
    }
    if (!toTrash) {
        // The name and not what it points at: `unlink()` removes the link, so
        // `rm link-to-dir` is a removal this verb does do, and
        // `QFileInfo::isDir` alone follows the link and would have refused it.
        // `trash` is not asked: `gio trash` takes a directory.
        bool directories = false;
        for (const QString &path : resolved) {
            const QFileInfo named(path);
            if (named.isDir() && !named.isSymLink()) {
                fail(QStringLiteral("rm: %1 is a directory; rm takes files").arg(path));
                directories = true;
            }
        }
        if (directories && status == kOk)
            status = kUsage;
    }
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    // Counted in rows, not in arguments: `trash somedir` is one path and every
    // row beneath it, and a prompt whose whole purpose is informed consent has
    // to quote the number that matters.
    int rows = 0;
    for (const QString &path : resolved)
        rows += qMax(1, rowsUnder(s.store, path).size());
    if (!g.dryRun && !confirmed(g, verb, resolved, rows))
        return kUsage;

    QJsonArray done;
    QStringList gone;
    // Every operand, and then the report. `/usr/bin/rm y1 cdir y3` unlinks both
    // files it can and reports the one it cannot, exit 1; `chmod` and `chown`
    // do the same. What is left in this loop is only what the system itself
    // answers -- the refusals this verb decides for itself are settled above,
    // before the prompt.
    bool refused = false;
    for (const QString &path : resolved) {
        if (!g.dryRun) {
            if (toTrash) {
                QProcess gio;
                gio.start(QStringLiteral("gio"), {QStringLiteral("trash"), path});
                if (!gio.waitForFinished(15000) || gio.exitCode() != 0) {
                    // gio knows why and says so -- most often that the file is
                    // on a filesystem with no trash directory of its own and
                    // none can be made -- so the reader gets that rather than
                    // "failed".
                    const QString detail =
                        QString::fromLocal8Bit(gio.readAllStandardError()).trimmed();
                    if (gio.error() == QProcess::FailedToStart) {
                        // Checked on PATH before the prompt, so this is the
                        // race and not the ordinary case: gio went away between
                        // the two.
                        fail(QStringLiteral("trash: gio is not installed; it comes with "
                                            "glib2 and is what puts a file in the "
                                            "desktop's trash"));
                    } else {
                        fail(detail.isEmpty()
                                 ? QStringLiteral("trash: gio trash failed on %1").arg(path)
                                 : QStringLiteral("trash: %1").arg(detail));
                    }
                    refused = true;
                    continue;
                }
            } else if (!QFile::remove(path)) {
                fail(QStringLiteral("rm: cannot remove %1: %2")
                         .arg(path, QString::fromLocal8Bit(strerror(errno))));
                refused = true;
                continue;
            }
        }
        // A folder handed to the trash takes everything under it, and so must
        // its rows -- otherwise the index keeps tags for files that are in the
        // wastebasket.
        gone.append(rowsUnder(s.store, path));
        if (!gone.contains(path))
            gone.append(path);
    }
    gone.removeDuplicates();
    // The rows of everything that did go, whether or not something else did
    // not: a removal that failed halfway has still removed things, and the
    // index quietly keeping rows for files that are no longer there is the
    // exact state `prune` exists to clean up after.
    status = dropRows(g, &s.store, gone, &done);
    if (status != kOk)
        return status;
    if (g.json)
        printJson(QJsonObject{{QStringLiteral("removed"), done}});
    return refused ? kUsage : kOk;
}

// The tags in a value read off a file that are tags, and the ones that are not.
//
// `normalizeTags` is all or nothing, which is the right rule for a list
// somebody typed: `tag add --tag "Bad Tag"` refusing outright beats tagging
// half of what was asked for, because the person is there to retype it. It is
// the wrong rule for a value found on disk, where refusing `good,Bad Tag`
// outright leaves a state nothing here can repair. Git reads its mailmap this
// way: a line it cannot parse is dropped and the rest of the file still
// counts.
QStringList salvagedTags(const QStringList &raw, QStringList *rejected)
{
    QStringList clean;
    for (const QString &item : raw) {
        QString error;
        const QString tag = normalizeTag(item, &error);
        if (tag.isEmpty()) {
            if (rejected)
                rejected->append(item);
            continue;
        }
        clean.append(tag);
    }
    return uniqueSorted(clean);
}

// The xattr is the copy that travels with the file, so it is what a rebuild
// reads. Without `--from-xattr` the index wins where the two disagree and only
// the rows it never had are added; with it, the file's own tags replace what
// the index believed.
int cmdReindex(const Globals &g, Session &s, bool fromXattr, const QStringList &positionals)
{
    int status = kOk;
    QString dir = optionalDir(positionals, &status);
    if (status != kOk)
        return status;
    if (dir.isEmpty())
        dir = QDir::currentPath();

    if ((status = openSession(&s)) != kOk)
        return status;

    QVector<Record> written;
    // A path it walked away from is `s.incomplete`, which is where the exit
    // code below comes from. Only the note about tags it ignored and still
    // finished the path is a `warn`.
    QStringList unnamed;
    const QStringList walked = tagWalk(dir, true, nullptr, &unnamed);
    // A name no row can hold is a path this rebuild could not settle, and the
    // repair is a rename rather than anything here.
    for (const QString &mangled : unnamed) {
        fail(QStringLiteral("reindex: %1 cannot be named; the name is not valid UTF-8, "
                            "so no index row can hold it -- rename it").arg(mangled));
        s.incomplete = true;
    }
    for (const QString &path : walked) {
        QStringList tags;
        QString warning;
        if (!readTagsXattr(path, &tags, &warning)) {
            fail(warning);
            s.incomplete = true;
            continue;
        }

        // The row is looked up before anything short-circuits, or the
        // commonest repair is out of reach: a file copied without
        // `--preserve=xattr` has a row and no xattr, which `verify` calls
        // `xattr-differs`.
        const Record existing = s.store.get(path);
        // What the file says, as it says it, and what of that is usable. The
        // two are compared separately: `onDisk` is the value `verify` calls
        // `xattr-differs` over, so rewriting exactly when it differs is what
        // makes the pair converge.
        const QStringList onDisk = uniqueSorted(tags);
        QStringList rejected;
        const QStringList clean = salvagedTags(tags, &rejected);
        const bool unusable = !tags.isEmpty() && clean.isEmpty();
        if (!rejected.isEmpty() && !unusable) {
            warn(g, QStringLiteral("reindex: %1: ignored invalid tag(s): %2")
                        .arg(path, rejected.join(QStringLiteral(", "))));
        }

        const bool indexed = !existing.path.isEmpty();

        // The index keeps its answer, and then makes that true on disk --
        // whether the file's copy differs, is empty, or is something no tag
        // could be read out of.
        if (indexed && !fromXattr) {
            if (existing.tags != onDisk) {
                s.store.put(existing);           // unchanged; carries the stamp
                written.append(existing);
                mirrorTags(g, s, existing, true);
            }
            continue;
        }

        // The file's copy wins -- but it has to have something to say. An empty
        // xattr almost always means the filesystem did not carry it, not that
        // somebody cleared the tags, and taking it at its word here would empty
        // the index of every row whose file was copied badly.
        if (clean.isEmpty()) {
            // And when it did say something, nothing of which is a tag, the
            // answer names the repair: nothing here can invent a row out of
            // `Has Space!`, and a rebuild is not licensed to delete a value it
            // did not write.
            //
            // Every one of these is a path the run walked away from, so every
            // one of them is nonzero, whatever the reason it declined. Exiting
            // 0 here would make `reindex --from-xattr && verify` a check with
            // no fixed point: the verify could only fail, and nothing in the
            // pair would end it.
            if (unusable && indexed) {
                fail(QStringLiteral("reindex: %1: no tag can be read out of xattr %2; "
                                    "the index row is left alone -- plain `reindex` "
                                    "writes it back over the file's copy")
                         .arg(path, tags.join(QLatin1Char(','))));
                s.incomplete = true;
            } else if (unusable) {
                fail(QStringLiteral("reindex: %1: no tag can be read out of xattr %2 and "
                                    "there is no index row to fall back on; "
                                    "`omafiles tag clear -- %1` drops the value")
                         .arg(path, tags.join(QLatin1Char(','))));
                s.incomplete = true;
            } else if (indexed) {
                // Declining is right -- an empty xattr almost always means the
                // filesystem dropped the value, not that somebody cleared the
                // tags -- but the message has to name the command that does
                // converge, the way the two above do.
                fail(QStringLiteral("reindex: %1 has no tags of its own; the index "
                                    "row is left alone -- plain `reindex` writes it "
                                    "back over the file's copy").arg(path));
                s.incomplete = true;
            }
            continue;
        }

        QString error;
        const Record record = stamped(path, clean, &error, nullptr, existing.tagged);
        if (record.path.isEmpty()) {
            // Walked past between the walk and here, or the system stopped
            // talking about it. Either way this run did not settle it.
            fail(error);
            s.incomplete = true;
            continue;
        }
        // The row and the file both have to end up saying it. Comparing only
        // the row would let a file keep a value the row has been cleaned of --
        // row `good`, xattr `good,Bad Tag` -- which `verify` goes on calling
        // `xattr-differs`.
        if (existing.tags == record.tags && onDisk == record.tags)
            continue;
        s.store.put(record);
        written.append(record);
        // The file's own copy follows the row it mirrors, the way it does
        // everywhere else -- but only where the two do not already agree, the
        // same skip the plain path above takes. Writing a value straight back
        // onto the file it was just read from is a ctime bump for nothing on a
        // writable file, and on one the filesystem will not take a write for it
        // is exit 1 from a run that settled every path it walked.
        if (onDisk != record.tags)
            mirrorTags(g, s, record, true);
    }

    const int committed = finish(g, s, written);
    if (committed != kOk)
        return committed;
    // Says what it touched, for a verb whose whole job is to change rows -- but
    // not in a dry run, where `commitStore` is already printing the same list.
    //
    // Said after the commit, and never from inside a batch, the rule `prune`
    // keeps with its dropped list: a batch prints nothing until it commits, and
    // a later line that fails rolls the whole run back, so a path named early
    // is a phantom. The same order keeps a failed commit silent on stdout.
    //
    // `--quiet` is "No stderr except errors" and stdout is not stderr, so this
    // list stands. `prune`, `import` and `batch` all answer on stdout whatever
    // `--quiet` says.
    if (!g.json && !g.dryRun && !s.deferred) {
        for (const Record &record : written)
            out() << record.path << '\n';
    }
    // A run that walked away from a path did not do all of what it was asked,
    // and that is a nonzero exit everywhere else on the system -- `cp`, `chmod`
    // and `rm -f` all report one file they could not handle that way. A file
    // whose copy the filesystem would not take is one of those paths, and the
    // flush inside `finish` is where it was counted.
    //
    // Not from inside a batch, where a nonzero status means "roll the whole run
    // back". The batch reads `s.incomplete` after its own commit instead.
    return s.incomplete && !s.deferred ? kIncomplete : kOk;
}

// ------------------------------------------------------------------- sweep

// How long `sweep` waits for another writer before it gives up.
//
// Every other verb waits five seconds and then exits 3, which is right for a
// command somebody typed and watched. This is the one meant to run when nobody
// asked it to, so it does the opposite: a second, and a lock it does not get is
// not a failure -- it is a run that stepped aside, and the next one does the
// same work over the same trees. Anything else would mean that tagging a file
// in the window while a sweep happened to be walking either stalled the window
// or mailed somebody an exit 3, and a routine that does either is a routine
// that gets turned off.
constexpr int kSweepLockWait = 1000;

// Where the last run's receipt lives.
//
// State, and neither data nor a declaration: it records that something
// happened rather than what the index or the roots are, losing it costs only
// the answer to "when did this last run", and nothing reads it back into a
// decision. `$XDG_STATE_HOME` is the directory the basedir spec names for
// exactly that.
QString sweepReceiptPath()
{
    QString stateHome = QString::fromLocal8Bit(qgetenv("XDG_STATE_HOME"));
    if (stateHome.isEmpty())
        stateHome = QDir::homePath() + QStringLiteral("/.local/state");
    return QDir(stateHome).filePath(QStringLiteral("omafiles/sweep.json"));
}

// What one run found. The counts are what the receipt keeps; the paths are what
// the run itself prints, because a receipt naming every path would grow without
// bound and the run has already said them.
struct SweepRun {
    QStringList trees;
    QStringList adopted;      // a file carrying tags the index had never heard of
    QStringList merged;       // row and file made to agree, one gaining from the other
    QStringList gone;         // a row whose file is not there -- counted, never dropped
    QStringList unreadable;   // a file that is there and whose tags are not readable
    int skipped = 0;          // a row on a disk in a drawer, or one nobody may look at
    int listed = 0;           // paths written to a root's list of names
    int unlistable = 0;       // names the list's one-path-to-a-line format cannot hold
};

QJsonArray asJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

// One tree, under one hold of the lock.
//
// The hold is the whole reason this is a pass and not a walk: the window writes
// through this same CLI now, so a sweep that held the index across every
// declared root would surface as `store: cannot lock` in somebody's row menu.
// One tree, one lock, one commit, and the lock goes back between them.
void sweepOneTree(const Globals &g, Session &s, const QString &tree, SweepRun *run,
                  QVector<Record> *written, const Root *cacheFor)
{
    // Asked before the walk, so a row this run is about to write is not then
    // looked up as a row whose file might be gone.
    for (const Record &record : s.store.records()) {
        if (!isUnderDirectory(record.path, tree))
            continue;
        QString error;
        const PathState state = pathState(record.path, &error);
        if (state == PathState::Present)
            continue;
        // "I may not look" is not "it is gone", and neither is "the disk is in
        // a drawer" -- the distinction `prune` turns on, and this verb counts
        // rather than acts on either way.
        if (state == PathState::Missing)
            run->gone.append(record.path);
        else
            ++run->skipped;
    }

    // The walk is the expensive half of this verb and two things want it: the
    // tags each file carries, and the plain list of what is there. Collecting
    // the second costs a string per path and saves a second walk of the disk --
    // see `Names.h` for what that list is and is not.
    QStringList names;
    const QString base = tree.endsWith(QLatin1Char('/')) ? tree : tree + QLatin1Char('/');
    for (const QString &path : tagWalk(tree, true)) {
        if (cacheFor && path.startsWith(base))
            names.append(path.mid(base.size()));
        QStringList carried;
        QString warning;
        if (!readTagsXattr(path, &carried, &warning)) {
            run->unreadable.append(path);
            warn(g, warning);
            continue;
        }
        QStringList rejected;
        const QStringList clean = salvagedTags(carried, &rejected);
        const Record existing = s.store.get(path);
        const bool indexed = !existing.path.isEmpty();
        if (!indexed && clean.isEmpty())
            continue;                       // nothing on either side to work from

        // The one rule the whole verb is: the union, both ways, and never a
        // removal. The row keeps every tag it holds and gains every tag the
        // file carries, which is what makes a file moved, copied or restored
        // by something else come home on its own; and it is why this can be
        // left running unwatched, where `reindex` -- which lets one side win
        // outright -- cannot.
        QStringList united = existing.tags;
        united.append(clean);
        united = uniqueSorted(std::move(united));

        bool changed = false;
        if (!indexed || united != existing.tags) {
            QString error;
            const Record record = stamped(path, united, &error, nullptr, existing.tagged);
            if (record.path.isEmpty()) {
                // Walked past between the walk and here. Not something to
                // report at: the next run will find it or it will not be there.
                ++run->skipped;
                continue;
            }
            s.store.put(record);
            written->append(record);
            changed = true;
            (indexed ? run->merged : run->adopted).append(path);
        }

        // And the file's own copy gains whatever the row holds and it does not
        // -- the other half of the union, so `verify` stops calling the pair
        // `xattr-differs` rather than being told about it every run.
        //
        // Except over a value this could not read whole. Writing the salvaged
        // tags back over `good,Bad Tag` would delete `Bad Tag` from the file,
        // and a verb that may not remove a tag may not remove one by rewriting
        // the value it lives in either. `verify` goes on reporting that one,
        // and `tag clear` is the verb licensed to drop it.
        if (rejected.isEmpty() && uniqueSorted(carried) != united) {
            Record mirror;
            mirror.path = path;
            mirror.tags = united;
            mirrorTags(g, s, mirror);
            if (!changed)
                run->merged.append(path);
        }
    }

    // Written outside the store entirely -- a different file, in a different
    // directory, under no lock. That is the point: the walk can be long without
    // anybody's `tag add` waiting on it, which is what makes a cache built by
    // the routine affordable at all.
    //
    // Only for a tree that is exactly a declared root. The list is per root and
    // a walk of one folder inside one cannot produce it; writing a partial list
    // would be worse than having none, because `--cached` would answer out of
    // it and call the answer whole.
    if (!cacheFor || g.dryRun)
        return;
    int dropped = 0;
    QString error;
    if (!writeNames(cacheFor->id, names, &dropped, &error)) {
        warn(g, error);
        return;
    }
    run->listed += names.size() - dropped;
    run->unlistable += dropped;
}

// Which trees a run sweeps.
//
// Named, or every declared root that is here. The second is the answer that
// matters: a run nobody is watching cannot take its scope from where it was
// started, and `roots ls` is already the list of the trees this machine keeps
// tags on. A machine that has declared none has no such list, so it is asked
// for a directory rather than handed the current one.
QStringList sweepTrees(const QStringList &positionals, int *status)
{
    QString dir = optionalDir(positionals, status);
    if (*status != kOk)
        return {};
    if (!dir.isEmpty()) {
        if (cannotLookInto(dir)) {
            fail(QStringLiteral("sweep: %1 is unreadable; nothing under it was swept")
                     .arg(dir));
            *status = kIncomplete;
            return {};
        }
        return {dir};
    }
    if (roots().isEmpty()) {
        fail(QStringLiteral("sweep: no directory was named and no root is declared; "
                            "name one -- `omafiles sweep <dir>` -- or declare the trees "
                            "this machine keeps tags on with `omafiles roots add`"));
        *status = kUsage;
        return {};
    }
    QStringList trees;
    for (const Root &root : roots()) {
        // A root that is not here is not a failure and never was: that is the
        // disk in the drawer this whole distinction exists for.
        if (!rootIsReachable(root))
            continue;
        QString canonical = canonicalPath(root.path, nullptr);
        trees.append(canonical.isEmpty() ? root.path : canonical);
    }
    return trees;
}

int printSweepStatus(const Globals &g, const QStringList &positionals)
{
    if (!positionals.isEmpty()) {
        fail(QStringLiteral("sweep: --status reports the last run; it takes no directory"));
        return kUsage;
    }
    QFile file(sweepReceiptPath());
    QJsonObject receipt;
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject())
            receipt = doc.object();
    }
    // A receipt nobody has written yet is an answer and not an error: it says
    // the routine has never run, which is what somebody asking is asking.
    if (receipt.isEmpty()) {
        if (g.json) {
            printJson(QJsonObject{{QStringLiteral("at"), 0},
                                  {QStringLiteral("trees"), QJsonArray{}},
                                  {QStringLiteral("adopted"), 0},
                                  {QStringLiteral("merged"), 0},
                                  {QStringLiteral("gone"), 0},
                                  {QStringLiteral("unreadable"), 0},
                                  {QStringLiteral("skipped"), 0},
                                  {QStringLiteral("listed"), 0}});
        } else {
            out() << "never\n";
        }
        return kOk;
    }
    if (g.json) {
        printJson(receipt);
        return kOk;
    }
    // The shape `tags` and `roots ls` print in: one fact to a line, tab
    // separated, so `cut -f2` gets the value out of any of them.
    const qint64 at = static_cast<qint64>(receipt.value(QStringLiteral("at")).toDouble());
    // Local wall clock carrying its own offset. `Qt::ISODate` over a local
    // QDateTime prints no offset at all, which is a timestamp nobody can place
    // once the machine has travelled or the clocks have gone back.
    const QDateTime when = QDateTime::fromSecsSinceEpoch(at);
    out() << "at\t" << when.toOffsetFromUtc(when.offsetFromUtc()).toString(Qt::ISODate)
          << '\n';
    out() << "seconds\t"
          << QString::number(receipt.value(QStringLiteral("seconds")).toDouble(), 'f', 1)
          << '\n';
    const QJsonArray trees = receipt.value(QStringLiteral("trees")).toArray();
    for (const QJsonValue &tree : trees)
        out() << "tree\t" << tree.toString() << '\n';
    for (const char *field : {"adopted", "merged", "gone", "unreadable", "skipped",
                              "listed"}) {
        out() << field << '\t'
              << receipt.value(QString::fromLatin1(field)).toInt() << '\n';
    }
    return kOk;
}

void writeSweepReceipt(const SweepRun &run, qint64 milliseconds)
{
    const QString path = sweepReceiptPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;                              // a receipt nobody could write is not the run
    const QJsonObject receipt{
        {QStringLiteral("at"), QDateTime::currentSecsSinceEpoch()},
        {QStringLiteral("seconds"), milliseconds / 1000.0},
        {QStringLiteral("trees"), asJsonArray(run.trees)},
        {QStringLiteral("adopted"), run.adopted.size()},
        {QStringLiteral("merged"), run.merged.size()},
        {QStringLiteral("gone"), run.gone.size()},
        {QStringLiteral("unreadable"), run.unreadable.size()},
        {QStringLiteral("skipped"), run.skipped},
        {QStringLiteral("listed"), run.listed}};
    file.write(QJsonDocument(receipt).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.commit();
}

// The run you do not watch.
//
// It is the routine as a verb, so there is one implementation and a timer, if
// there is ever a timer, is three lines calling this. Two rules make it safe to
// leave alone: it only ever makes the index know more -- see the union in
// `sweepOneTree` -- and it steps aside rather than making anybody wait. What it
// finds and will not touch, it counts and names the verb for: a row whose file
// is gone is `prune`'s to drop, and never this one's.
int cmdSweep(const Globals &g, Session &s, bool status, const QStringList &positionals)
{
    if (status) {
        // A batch prints nothing until it commits, so a read in the middle of
        // one could only answer about rows a later line might still take away.
        // Every other read verb is refused by name up there; this one is a
        // flag, so it is refused here.
        if (s.deferred) {
            fail(QStringLiteral("batch: sweep --status only reads; run it on its own"));
            return kUsage;
        }
        return printSweepStatus(g, positionals);
    }

    int scopeStatus = kOk;
    const QStringList trees = sweepTrees(positionals, &scopeStatus);
    if (scopeStatus != kOk)
        return scopeStatus;
    if (trees.isEmpty()) {
        // Roots are declared and not one of them is here. Every disk in a
        // drawer at once is unusual and not wrong, and there is nothing to
        // report about it but that nothing was swept.
        warn(g, QStringLiteral("sweep: no declared root is here; nothing was swept"));
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("trees"), QJsonArray{}}});
        return kOk;
    }

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    SweepRun run;
    QStringList untouched;
    for (int i = 0; i < trees.size(); ++i) {
        const QString &tree = trees.at(i);
        QString error;
        if (s.deferred) {
            // Inside a `batch` the lock, the commit and the receipt all belong
            // to the batch, and there is nothing to take or give back here.
        } else if (g.dryRun) {
            if (!s.opened && !s.store.load(&error)) {
                fail(error);
                return kStore;
            }
        } else {
            s.store.setLockTimeout(kSweepLockWait);
            if (!s.store.lockAndLoad(&error)) {
                untouched = trees.mid(i);
                break;
            }
        }
        s.opened = true;

        QVector<Record> written;
        // A list of names belongs to a root, so it is written only where the
        // tree being swept is one. `sweep` with no `[dir]` is every declared
        // root, which is how the caches get built without anybody asking.
        const Root *declared = rootFor(tree);
        if (declared && declared->path != tree)
            declared = nullptr;
        sweepOneTree(g, s, tree, &run, &written, declared);
        run.trees.append(tree);
        // `announce` is false because the run has one answer for every tree it
        // swept and prints it below; a per-tree array would be one JSON
        // document per pass on a stdout that carries one.
        const int committed = finish(g, s, written, false);
        if (committed != kOk)
            return committed;
    }

    if (!untouched.isEmpty()) {
        warn(g, QStringLiteral("sweep: another writer holds the index; stepped aside "
                               "with %1 tree(s) unswept -- %2")
                    .arg(untouched.size())
                    .arg(untouched.join(QStringLiteral(", "))));
    }
    if (!g.dryRun && !s.deferred && !run.trees.isEmpty())
        writeSweepReceipt(run, QDateTime::currentMSecsSinceEpoch() - started);

    // Nothing at all from inside a batch, in either shape: the batch has one
    // answer for the whole run and prints it after its own commit.
    if (!s.deferred) {
        if (g.json) {
            printJson(QJsonObject{{QStringLiteral("trees"), asJsonArray(run.trees)},
                                  {QStringLiteral("adopted"), asJsonArray(run.adopted)},
                                  {QStringLiteral("merged"), asJsonArray(run.merged)},
                                  {QStringLiteral("gone"), asJsonArray(run.gone)},
                                  {QStringLiteral("unreadable"), asJsonArray(run.unreadable)},
                                  {QStringLiteral("skipped"), run.skipped},
                                  {QStringLiteral("listed"), run.listed},
                                  {QStringLiteral("unlistable"), run.unlistable}});
        } else {
            QStringList touched = run.adopted + run.merged;
            touched.sort();
            for (const QString &path : touched)
                out() << path << '\n';
        }
    }

    // The summary is a note beside a run that succeeded, which is what stderr
    // is for here and what `--quiet` is documented to take away. The counts are
    // the whole point of the verb -- what it changed, and what it found and
    // deliberately did not.
    QStringList parts{QStringLiteral("adopted %1").arg(run.adopted.size()),
                      QStringLiteral("merged %1").arg(run.merged.size())};
    if (!run.gone.isEmpty())
        parts << QStringLiteral("gone %1").arg(run.gone.size());
    if (!run.unreadable.isEmpty())
        parts << QStringLiteral("unreadable %1").arg(run.unreadable.size());
    if (run.skipped > 0)
        parts << QStringLiteral("skipped %1").arg(run.skipped);
    if (run.listed > 0)
        parts << QStringLiteral("listed %1").arg(run.listed);
    if (run.unlistable > 0)
        parts << QStringLiteral("unlistable %1").arg(run.unlistable);
    warn(g, QStringLiteral("sweep: %1").arg(parts.join(QStringLiteral(", "))));
    if (!run.gone.isEmpty()) {
        warn(g, QStringLiteral("sweep: a row whose file is gone is counted here and "
                               "never dropped; `omafiles prune` drops them"));
    }
    // Exit 0 whatever it counted. A file nobody may read and a row whose file
    // is gone are both findings rather than failures of the run, and a routine
    // whose status is nonzero every week over the same chmod is a routine whose
    // status nobody reads. `verify` is the verb that exits 1 over a finding.
    return kOk;
}

// A detail stays one field on one line whatever it quotes. It carries the bytes
// a file's `user.omafiles.tags` holds, and those are anybody's, so a tab is
// written `\t`, a newline `\n`, and a backslash `\\` so the first two cannot be
// forged. The path beside it is not escaped and cannot be: cut that field out
// of the line and it has to still be a path something can be handed.
QString escapedDetail(const QString &detail)
{
    QString text;
    text.reserve(detail.size());
    for (const QChar c : detail) {
        if (c == QLatin1Char('\\'))
            text += QLatin1String("\\\\");
        else if (c == QLatin1Char('\t'))
            text += QLatin1String("\\t");
        else if (c == QLatin1Char('\n'))
            text += QLatin1String("\\n");
        else
            text += c;
    }
    return text;
}

// Exit 1 when it found something, the way `diff` does: a check that always
// succeeds is a check nothing can be built on.
// The saved ways of asking, by name.
//
// A folder, a tag and a query are one thing here: a view is either a place or a
// set of tags, and both of them are `ls` with something filled in. That is why
// pinning a folder needed no feature of its own -- it is a view with a `dir`.
int cmdView(const Globals &g, Session &s, const QString &sub, const QString &name,
            const QStringList &tags, const QStringList &positionals)
{
    Q_UNUSED(s)
    const QString file = defaultViewsPath();
    QVector<View> views;
    QString error;
    if (!loadViews(file, &views, &error)) {
        fail(error);
        return kStore;
    }

    if (sub.isEmpty() || sub == QLatin1String("ls")) {
        if (g.json) {
            QJsonArray array;
            for (const View &view : std::as_const(views)) {
                QJsonArray viewTags;
                for (const QString &tag : view.tags)
                    viewTags.append(tag);
                array.append(QJsonObject{
                    {QStringLiteral("name"), view.name},
                    {QStringLiteral("dir"), view.dir},
                    {QStringLiteral("tags"), viewTags},
                });
            }
            printJson(array);
        } else {
            // The name, then what it asks -- as the command line that asks it,
            // so a view is a thing you can read as well as open.
            for (const View &view : std::as_const(views)) {
                QString asks = QStringLiteral("ls");
                for (const QString &tag : view.tags)
                    asks += QStringLiteral(" --tag ") + tag;
                if (!view.dir.isEmpty())
                    asks += QLatin1Char(' ') + view.dir;
                out() << view.name << '\t' << asks << '\n';
            }
        }
        return kOk;
    }

    if (sub == QLatin1String("add")) {
        if (name.isEmpty()) {
            fail(QStringLiteral("view add: --name is required"));
            return kUsage;
        }
        const QString clean = normalizeViewName(name, &error);
        if (clean.isEmpty()) {
            fail(error);
            return kUsage;
        }
        if (tags.isEmpty() && positionals.isEmpty()) {
            fail(QStringLiteral("view add: a view asks something; give it a --tag or a folder"));
            return kUsage;
        }
        // Refused rather than half-honoured. A tag query scoped to a folder is
        // a question the window cannot walk -- a tags tab is not standing
        // anywhere -- and a view that means one thing at a prompt and another
        // in a panel is worse than one that says it cannot.
        if (!tags.isEmpty() && !positionals.isEmpty()) {
            fail(QStringLiteral("view add: a view is a folder or a set of tags, not both; "
                                "the window has no way to walk a tag query inside a folder"));
            return kUsage;
        }
        if (positionals.size() > 1) {
            fail(QStringLiteral("view add: takes at most one folder"));
            return kUsage;
        }

        View view;
        view.name = clean;
        view.tags = tags;
        if (!positionals.isEmpty()) {
            PathState state = PathState::Missing;
            view.dir = canonicalPath(positionals.first(), &error, &state);
            if (view.dir.isEmpty()) {
                fail(error);
                return pathStatus(state);
            }
            if (!QFileInfo(view.dir).isDir()) {
                fail(QStringLiteral("view add: %1 is not a folder").arg(view.dir));
                return kUsage;
            }
        }

        // Replaced rather than added beside: a panel with two entries reading
        // `boletos` is a panel you have to open both of to tell apart.
        bool replaced = false;
        for (View &already : views) {
            if (already.name.compare(clean, Qt::CaseInsensitive) == 0) {
                already = view;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            views.append(view);

        if (g.dryRun) {
            out() << clean << '\n';
            return kOk;
        }
        if (!saveViews(file, views, &error)) {
            fail(error);
            return kStore;
        }
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("name"), clean}});
        else
            out() << clean << '\n';
        return kOk;
    }

    if (sub == QLatin1String("rm")) {
        if (name.isEmpty()) {
            fail(QStringLiteral("view rm: --name is required"));
            return kUsage;
        }
        QVector<View> kept;
        for (const View &view : std::as_const(views)) {
            if (view.name.compare(name, Qt::CaseInsensitive) != 0)
                kept.append(view);
        }
        if (kept.size() == views.size()) {
            fail(QStringLiteral("view rm: no view called %1").arg(name));
            return kUsage;
        }
        if (g.dryRun)
            return kOk;
        if (!saveViews(file, kept, &error)) {
            fail(error);
            return kStore;
        }
        return kOk;
    }

    fail(QStringLiteral("unknown command: view %1 (try omafiles view --help)").arg(sub));
    return kUsage;
}

// The declarations, and the two ways to change them.
//
// Read as well as written here rather than left to a text editor, because the
// interesting half of the answer is not in the file: whether the storage is
// actually here right now is a question about this machine at this moment, and
// it is the one that decides whether `prune` may touch the rows under it.
int cmdRoots(const Globals &g, Session &s, const QString &sub, const QString &id,
             bool mount, const QString &check, const QStringList &positionals)
{
    const QString file = defaultRootsPath();
    QVector<Root> declared = roots();

    if (sub.isEmpty() || sub == QLatin1String("ls")) {
        if (g.json) {
            QJsonArray array;
            for (const Root &root : declared) {
                array.append(QJsonObject{
                    {QStringLiteral("id"), root.id},
                    {QStringLiteral("path"), root.path},
                    {QStringLiteral("mount"), root.mount},
                    {QStringLiteral("check"), root.check},
                    {QStringLiteral("here"), rootIsReachable(root)},
                });
            }
            printJson(array);
        } else {
            for (const Root &root : declared) {
                out() << root.id << '\t' << root.path << '\t'
                      << (rootIsReachable(root) ? "here" : "away") << '\n';
            }
        }
        return kOk;
    }

    if (sub == QLatin1String("add")) {
        if (positionals.size() != 1) {
            fail(QStringLiteral("roots add: takes one path"));
            return kUsage;
        }
        if (id.isEmpty()) {
            fail(QStringLiteral("roots add: --id is required"));
            return kUsage;
        }
        QString error;
        // The id is a name and not a path: every index row names its root, and
        // the name cache is one file per root with this for a file name.
        const QString wanted = normalizeRootId(id, &error);
        if (wanted.isEmpty()) {
            fail(error);
            return kUsage;
        }
        // Resolved, because everything compared against it is: a root declared
        // through a symlink would hold no rows at all.
        PathState state = PathState::Missing;
        const QString path = canonicalPath(positionals.first(), &error, &state);
        if (path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        if (!QFileInfo(path).isDir()) {
            fail(QStringLiteral("roots add: %1 is not a directory").arg(path));
            return kUsage;
        }
        for (const Root &root : declared) {
            if (root.id == wanted) {
                fail(QStringLiteral("roots add: %1 is already declared, at %2")
                         .arg(wanted, root.path));
                return kUsage;
            }
            if (root.path == path) {
                fail(QStringLiteral("roots add: %1 is already declared, as %2")
                         .arg(path, root.id));
                return kUsage;
            }
        }
        declared.append(Root{wanted, path, mount, check});
        if (!saveRoots(file, declared, &error)) {
            fail(error);
            return kStore;
        }
        setRoots(declared);
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("id"), wanted},
                                  {QStringLiteral("path"), path}});
        else
            out() << path << '\n';
        return kOk;
    }

    if (sub == QLatin1String("rm")) {
        if (id.isEmpty()) {
            fail(QStringLiteral("roots rm: --id is required"));
            return kUsage;
        }
        // Folded the way `add` folded it, so `--id Archive` reaches the root
        // it declared -- but only after the spelling as typed has been tried,
        // because a declaration written before the rule existed is still one
        // somebody has to be able to remove.
        const QString wanted = rootById(id) ? id : normalizeRootId(id, nullptr);
        if (wanted.isEmpty() || !rootById(wanted)) {
            fail(QStringLiteral("roots rm: %1 is not declared").arg(id));
            return kUsage;
        }
        // The rows under it come with it, rewritten as the absolute paths they
        // resolve to now. A row names a declaration, so leaving them behind
        // would leave rows nothing can place and every verb refusing to read
        // the index until the line came back -- and that is not a state a
        // command should be able to put somebody in.
        //
        // The index is committed before the config is written. That way the
        // half-done case is the harmless one: rows that no longer name the
        // root, beside a declaration nothing references.
        int status = kOk;
        if ((status = openSession(&s)) != kOk)
            return status;

        QVector<Root> kept;
        for (const Root &root : declared) {
            if (root.id != wanted)
                kept.append(root);
        }
        const QVector<Root> before = setRoots(kept);

        int carried = 0;
        for (const Record &record : s.store.records()) {
            if (record.root != wanted)
                continue;
            Record rewritten = record;
            rewritten.root.clear();
            rewritten.relative.clear();
            s.store.put(rewritten);
            ++carried;
        }

        QString error;
        if (carried > 0 && !g.dryRun && !s.store.commit(&error)) {
            setRoots(before);
            fail(error);
            return kStore;
        }
        if (g.dryRun) {
            setRoots(before);
            if (g.json)
                printJson(QJsonObject{{QStringLiteral("id"), wanted},
                                      {QStringLiteral("rows"), carried}});
            else
                out() << carried << " row(s) would be written back as absolute paths\n";
            return kOk;
        }
        if (!saveRoots(file, kept, &error)) {
            fail(error);
            return kStore;
        }
        // The list of names goes with the declaration: it is one file per root,
        // and a root nobody declares is one nothing will ever read a list for.
        // Losing it costs a sweep and no information, which is what a cache is.
        forgetNames(wanted);
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("id"), wanted},
                                  {QStringLiteral("rows"), carried}});
        else if (carried > 0)
            out() << carried << " row(s) written back as absolute paths\n";
        return kOk;
    }

    if (sub == QLatin1String("migrate")) {
        // A read and a write of the same rows, which is all a migration is: the
        // form on disk is decided when a row is written, so every row is
        // rewritten under whichever root holds it now -- or, for one whose root
        // has gone away, back into the absolute path it can still be placed by.
        int status = kOk;
        if ((status = openSession(&s)) != kOk)
            return status;
        int rooted = 0;
        int absolute = 0;
        QStringList written;
        for (const Record &record : s.store.records()) {
            QString rootId;
            QString rest;
            splitByRoot(record.path, &rootId, &rest);
            if (rootId.isEmpty())
                ++absolute;
            else
                ++rooted;
            written.append(record.path);
            // Put back so the row is written in the form `toJson` decides now,
            // rather than the one it was read in.
            Record rewritten = record;
            rewritten.root = rootId;
            rewritten.relative = rest;
            s.store.put(rewritten);
        }
        if (g.dryRun) {
            if (g.json)
                printJson(QJsonObject{{QStringLiteral("rooted"), rooted},
                                      {QStringLiteral("absolute"), absolute}});
            else
                out() << rooted << " under a root, " << absolute << " absolute\n";
            return kOk;
        }
        QString error;
        if (!s.store.commit(&error)) {
            fail(error);
            return kStore;
        }
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("rooted"), rooted},
                                  {QStringLiteral("absolute"), absolute}});
        else
            out() << rooted << " under a root, " << absolute << " absolute\n";
        return kOk;
    }

    fail(QStringLiteral("unknown command: roots %1 (try omafiles roots --help)").arg(sub));
    return kUsage;
}

int cmdVerify(const Globals &g, Session &s, const QStringList &positionals)
{
    int status = kOk;
    const QString dir = optionalDir(positionals, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    // One answer to "where": `verify` and `verify .` are the same question, so
    // both sides take the same root rather than the disk walk falling back to
    // the current directory while the index side checks every row.
    const QString root = dir.isEmpty() ? QDir::currentPath() : dir;

    QJsonArray problems;
    int found = 0;
    QSet<QString> seen;
    const auto report = [&](const char *kind, const QString &path, const QString &detail) {
        ++found;
        if (g.json) {
            problems.append(QJsonObject{{QStringLiteral("problem"),
                                         QString::fromLatin1(kind)},
                                        {QStringLiteral("path"), path},
                                        {QStringLiteral("detail"), detail}});
        } else {
            out() << QString::fromLatin1(kind) << '\t' << path;
            if (!detail.isEmpty())
                out() << '\t' << escapedDetail(detail);
            out() << '\n';
        }
    };

    // The other direction: walking only the index would be blind to every file
    // carrying tags the index never heard of, which is what a lost write, or a
    // copy that brought its attributes along, leaves behind.
    //
    // Taken first because the index side below needs its answer: a directory
    // nobody may open fails its xattr read for that one reason, and reporting
    // the consequence there and the cause here is two problems where there is
    // one.
    QStringList shut;
    QStringList unnamed;
    QStringList unreachable;
    const QStringList disk = tagWalk(root, true, &shut, &unnamed);
    const QSet<QString> closedDirs(shut.cbegin(), shut.cend());

    for (const Record &record : s.store.records()) {
        if (!isUnderDirectory(record.path, root))
            continue;
        QString error;
        const PathState state = pathState(record.path, &error);
        // A row on a root nobody has plugged in is not a problem this run
        // found: there is nothing to compare it against, and calling it
        // `missing` would send somebody to `prune`, which is the one command
        // that would turn an unplugged drive into lost tags. Said as a note
        // instead, so `--quiet` takes it away and a healthy tree with a drive
        // in a drawer still exits 0.
        if (state == PathState::Unreachable) {
            unreachable.append(error);
            continue;
        }
        // Gone and unanswerable are different problems with different repairs:
        // one wants `prune`, the other a `chmod` or a mount. `unreadable` is
        // the kind this verb uses for a directory it could not look inside; a
        // file it could not stat is the same one level down.
        if (state == PathState::Missing) {
            report("missing", record.path, QString());
            continue;
        }
        // Marked seen before anything can skip out, or a file nobody may read
        // is reported by this loop and again by the disk walk below -- the same
        // object twice under `--json`.
        seen.insert(record.path);
        if (state == PathState::Unknown) {
            report("unreadable", record.path, error);
            continue;
        }
        // And a shut directory that happens to be indexed says nothing here:
        // the walk reports it below, under the kind that also says the tree
        // beneath it went unchecked. Two kinds for one chmod is one too many.
        if (closedDirs.contains(record.path))
            continue;
        QStringList tags;
        QString warning;
        if (!readTagsXattr(record.path, &tags, &warning)) {
            report("xattr-unreadable", record.path, warning);
            continue;
        }
        const QStringList onDisk = uniqueSorted(tags);
        if (onDisk != record.tags) {
            report("xattr-differs", record.path,
                   QStringLiteral("index=%1 xattr=%2")
                       .arg(record.tags.join(QLatin1Char(',')),
                            onDisk.join(QLatin1Char(','))));
        }
    }

    for (const QString &closed : shut) {
        // What the walk did, not what nobody else could: rows the index already
        // holds under it are reported on their own lines, each unreadable for
        // this same reason, so a detail claiming nothing under it was looked at
        // would be false on the very lines beside it.
        report("unreadable", closed,
               QStringLiteral("the tree under it was not walked"));
        // Its xattr cannot be read either, for the same reason. Saying so twice
        // is two lines about one cause.
        seen.insert(closed);
    }
    for (const QString &mangled : unnamed) {
        // The same kind: a path this run cannot address, so it cannot say
        // anything about the path or the tree under it. The detail is what
        // separates the two causes, because the repairs differ -- a chmod or a
        // mount there, a rename here.
        report("unreadable", mangled,
               QStringLiteral("the name is not valid UTF-8; neither it nor the tree "
                              "under it was walked"));
    }
    for (const QString &canonical : disk) {
        if (seen.contains(canonical))
            continue;
        seen.insert(canonical);
        QStringList tags;
        QString warning;
        // The same problem the index side reports: throwing the warning away
        // would make a file nobody may read indistinguishable from a file with
        // no tags, the one distinction this verb exists to draw.
        if (!readTagsXattr(canonical, &tags, &warning)) {
            report("xattr-unreadable", canonical, warning);
            continue;
        }
        if (tags.isEmpty())
            continue;
        // `unindexed` is the kind that means "run reindex", and for a value no
        // tag can be read out of, reindex is not the answer -- it has no row to
        // fall back on and will not delete a value it did not write. Under the
        // repairable kind it would alarm forever with nothing to act on, so it
        // gets a kind of its own and names the command that ends it.
        if (salvagedTags(tags, nullptr).isEmpty()) {
            report("xattr-invalid", canonical,
                   QStringLiteral("xattr=%1; no tag can be read out of it -- "
                                  "`omafiles tag clear -- %2` drops the value")
                       .arg(tags.join(QLatin1Char(',')), canonical));
            continue;
        }
        report("unindexed", canonical,
               QStringLiteral("xattr=%1").arg(uniqueSorted(tags).join(QLatin1Char(','))));
    }

    // Said after the problems, because it is not one of them: a row under a
    // root that is not here was not checked, and that is a fact about the
    // machine rather than about the index.
    for (const QString &waiting : unreachable) {
        warn(g, QStringLiteral("verify: %1; it was not checked").arg(waiting));
    }

    if (g.json)
        printJson(QJsonObject{{QStringLiteral("problems"), problems}});
    return found == 0 ? kOk : 1;
}


struct Parsed {
    QStringList tags;
    QStringList anys;
    QStringList exts;
    QString since;
    // Whether the flag was typed, not whether it carried anything: `--since ""`
    // -- an unset `$WINDOW` in a script -- has to reach validation rather than
    // taking the filter away with it and listing the whole index. `--tag ""`
    // and `--any ""` are both refused.
    bool sinceGiven = false;
    QString from;
    QString id;
    QString check;
    QString name;
    QString replace;
    QString with;
    QString pattern;
    // Typed, as with `since`: `--with ""` is how the replaced text is deleted,
    // and `--replace ""` has to reach its own refusal.
    bool replaceGiven = false;
    bool withGiven = false;
    bool patternGiven = false;
    bool mount = false;
    bool untagged = false;
    bool cached = false;
    bool fromXattr = false;
    bool all = false;
    bool status = false;
    QStringList positionals;
};

int parseFlags(QStringList args, Parsed *parsed, const Takes &takes)
{
    const bool allowTag = takes.tag;
    const bool allowLsFlags = takes.lsFlags;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--")) {
            parsed->positionals.append(args.mid(i + 1));
            break;
        }
        if (arg == QLatin1String("-t") || arg == QLatin1String("--tag")) {
            if (!allowTag) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("%1 needs a value").arg(arg));
                return kUsage;
            }
            parsed->tags.append(value);
            continue;
        }
        if (allowLsFlags && arg == QLatin1String("--any")) {
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("--any needs a value"));
                return kUsage;
            }
            parsed->anys.append(value);
            continue;
        }
        if (allowLsFlags && arg == QLatin1String("--untagged")) {
            parsed->untagged = true;
            continue;
        }
        if (allowLsFlags && arg == QLatin1String("--cached")) {
            parsed->cached = true;
            continue;
        }
        if (allowLsFlags && arg == QLatin1String("--ext")) {
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("--ext needs a value"));
                return kUsage;
            }
            parsed->exts.append(normalizeExt(value));
            continue;
        }
        if (allowLsFlags && arg == QLatin1String("--since")) {
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("--since needs a value"));
                return kUsage;
            }
            parsed->since = value;
            parsed->sinceGiven = true;
            continue;
        }
        if (arg == QLatin1String("--name")) {
            if (!takes.view) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("--name needs a value"));
                return kUsage;
            }
            parsed->name = value;
            continue;
        }
        if (arg == QLatin1String("--id") || arg == QLatin1String("--check")) {
            if (!takes.root) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("%1 needs a value").arg(arg));
                return kUsage;
            }
            if (arg == QLatin1String("--id"))
                parsed->id = value;
            else
                parsed->check = value;
            continue;
        }
        if (arg == QLatin1String("--mount")) {
            if (!takes.root) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            parsed->mount = true;
            continue;
        }
        if (arg == QLatin1String("--from")) {
            if (!takes.from) {
                // It eats the next word, so ungated, `ls --from --tag work`
                // would swallow `--tag` and go looking for a file called
                // `work`.
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("--from needs a value"));
                return kUsage;
            }
            parsed->from = value;
            continue;
        }
        if (arg == QLatin1String("--from-xattr")) {
            if (!takes.fromXattr) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            parsed->fromXattr = true;
            continue;
        }
        if (arg == QLatin1String("--replace") || arg == QLatin1String("--with")
            || arg == QLatin1String("--pattern")) {
            if (!takes.rename) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            QString value;
            if (!consumeValue(args, &i, &value)) {
                fail(QStringLiteral("%1 needs a value").arg(arg));
                return kUsage;
            }
            if (arg == QLatin1String("--replace")) {
                parsed->replace = value;
                parsed->replaceGiven = true;
            } else if (arg == QLatin1String("--with")) {
                parsed->with = value;
                parsed->withGiven = true;
            } else {
                parsed->pattern = value;
                parsed->patternGiven = true;
            }
            continue;
        }
        if (arg == QLatin1String("--status")) {
            if (!takes.status) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            parsed->status = true;
            continue;
        }
        if (arg == QLatin1String("--all")) {
            if (!takes.all) {
                fail(QStringLiteral("unexpected %1").arg(arg));
                return kUsage;
            }
            parsed->all = true;
            continue;
        }
        if (arg.startsWith(QLatin1Char('-'))) {
            fail(QStringLiteral("unknown flag: %1").arg(arg));
            return kUsage;
        }
        parsed->positionals.append(arg);
    }
    return kOk;
}

// ------------------------------------------------------------------- batch

int dispatch(const Globals &g, Session &s, QStringList rest);

// POSIX-ish word splitting, because the lines of a command file are mostly
// paths and real file names have spaces in them. Splitting on whitespace alone
// would be a bug shipped on purpose: `tag add --tag x -- /home/me/tax return.pdf`
// would silently become two paths, neither of which exists.
//
// Single quotes take everything to the next one literally. Double quotes do the
// same except that a backslash before a quote or another backslash escapes it.
// Unquoted, a backslash escapes whatever follows.
bool splitLine(const QString &line, QStringList *words, QString *error)
{
    QString word;
    bool started = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('\'')) {
            started = true;
            for (++i; i < line.size() && line.at(i) != QLatin1Char('\''); ++i)
                word.append(line.at(i));
            if (i >= line.size()) {
                *error = QStringLiteral("unterminated single quote");
                return false;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            started = true;
            for (++i; i < line.size() && line.at(i) != QLatin1Char('"'); ++i) {
                const QChar here = line.at(i);
                if (here == QLatin1Char('\\') && i + 1 < line.size()
                    && (line.at(i + 1) == QLatin1Char('"')
                        || line.at(i + 1) == QLatin1Char('\\')))
                    ++i;
                word.append(line.at(i));
            }
            if (i >= line.size()) {
                *error = QStringLiteral("unterminated double quote");
                return false;
            }
            continue;
        }
        if (c == QLatin1Char('\\')) {
            if (i + 1 >= line.size()) {
                *error = QStringLiteral("line ends in a backslash");
                return false;
            }
            word.append(line.at(++i));
            started = true;
            continue;
        }
        if (c.isSpace()) {
            if (started) {
                words->append(word);
                word.clear();
                started = false;
            }
            continue;
        }
        word.append(c);
        started = true;
    }
    if (started)
        words->append(word);
    return true;
}

// What a batch will take, and why it will not take the rest.
//
// A batch promises that either every line landed or none of them did, and that
// promise only reaches as far as the index. `mv`, `cp`, `rm` and `trash` move
// and delete real files, and rolling the jsonl back does not bring a deleted
// file home; a batch that half-emptied a directory and then said it had written
// nothing would be lying about the half that mattered.
//
// The read verbs are refused for the other half of the promise. A batch says
// nothing until it has committed, so an `ls` in the middle of one could only
// print rows a later line might still take away.
bool batchTakes(const QStringList &words, QString *why)
{
    for (const QString &word : words) {
        if (word == QLatin1String("--"))
            break;
        if (isGlobalFlag(word)) {
            *why = QStringLiteral("%1 is a flag for the batch itself, not for a line")
                       .arg(word);
            return false;
        }
    }

    const QString &cmd = words.first();
    static const QStringList filesystem = {QStringLiteral("mv"), QStringLiteral("cp"),
                                           QStringLiteral("rm"), QStringLiteral("trash")};
    if (filesystem.contains(cmd)) {
        *why = QStringLiteral("%1 changes files on disk and a rolled back index cannot "
                              "undo that; run it on its own")
                   .arg(cmd);
        return false;
    }

    const bool tagLs = cmd == QLatin1String("tag")
        && words.value(1) == QLatin1String("ls");
    if (onlyReads(cmd) || tagLs) {
        *why = QStringLiteral("%1 only reads, and a batch prints nothing until it commits; "
                              "run it on its own")
                   .arg(tagLs ? QStringLiteral("tag ls") : cmd);
        return false;
    }

    if (cmd == QLatin1String("batch")) {
        *why = QStringLiteral("batch cannot nest; one file, one store write");
        return false;
    }
    return true;
}

// Many commands over one index, read once and written once.
//
// Every line is run against the same in-memory store and the commit waits for
// the last of them, so twenty `tag add` lines cost one read and one write
// instead of twenty of each. It also makes the run atomic for free: a line that
// fails returns before anything reached the disk, and the jsonl is left exactly
// as it was found.
int cmdBatch(const Globals &g, Session &s, const QStringList &positionals)
{
    if (positionals.size() > 1) {
        fail(QStringLiteral("batch: one file at most"));
        return kUsage;
    }
    const QString source = positionals.isEmpty() ? QStringLiteral("-") : positionals.first();

    QFile file;
    int status = openInput(QStringLiteral("batch"), source, &file);
    if (status != kOk)
        return status;

    // The whole file is read before the first line runs. A command that asked
    // stdin a question halfway through would otherwise eat the lines that were
    // still waiting to be commands -- `prune` is one, so from a terminal it can
    // prompt in the middle of a run and the run survives it. Off a terminal it
    // refuses, and the batch fails with it; `--yes` on the batch answers for
    // every line at once.
    QStringList lines;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine());
        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        lines.append(line);
    }

    status = openSession(&s);
    if (status != kOk)
        return status;

    const auto refuse = [](int number, const QString &line, const QString &why) {
        fail(QStringLiteral("batch: line %1: %2").arg(number).arg(why));
        fail(QStringLiteral("  %1").arg(line));
    };

    s.deferred = true;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const QString trimmed = line.trimmed();
        // The first column and nowhere else: past it `#` is an ordinary
        // character, which is what makes a file called `#draft.md` nameable
        // without quoting it.
        if (trimmed.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        QStringList words;
        QString error;
        if (!splitLine(line, &words, &error)) {
            refuse(i + 1, line, error);
            return kUsage;
        }
        // QString::trimmed only strips ASCII whitespace, so a line holding
        // nothing but, say, a non-breaking space gets past the blank check above
        // and splits into no words at all.
        if (words.isEmpty())
            continue;
        QString why;
        if (!batchTakes(words, &why)) {
            refuse(i + 1, line, why);
            return kUsage;
        }
        status = dispatch(g, s, words);
        if (status != kOk) {
            refuse(i + 1, line, QStringLiteral("failed, nothing was written"));
            return status;
        }
    }

    s.deferred = false;
    status = commitStore(g, &s.store, committedRows(s.store, s.written));
    if (status != kOk || g.dryRun)
        return status;
    flushPending(g, s);
    // A line that aborts takes the whole run back above. This is the other
    // half: a line that ran to the end and did not settle what it promised --
    // `reindex` over a refused xattr, `tag clear` on a path with no row to
    // record the drop -- exits 1 on its own, and the batch says the same.
    //
    // The commit stands, because the rows landed and a diagnosis about what did
    // not settle is no reason to throw away what did. A refused mirror that
    // `tag add` promises nothing about is exit 0 here as it is on its own.
    return s.incomplete ? kIncomplete : kOk;
}

// One place that turns an argv tail into a verb, so a batch line runs the same
// code path the shell would have run and the two cannot drift apart.
// A set renamed by rule, each in its own folder.
//
// `--replace` and `--with` rewrite the name; `--pattern` then builds one out of
// what is left: `{name}` the whole of it, `{stem}` and `{ext}` its two halves
// -- the extension with its dot, and nothing where there is none -- and `{n}`
// the operand's place in the set, counted from one and padded to the width of
// the count. Every destination is settled before any rename is made, the way
// `rm` settles its list: a name already taken, or two operands landing on one
// name, refuses the whole set. A name held by an operand that is itself about
// to move is no exception -- do that in two passes -- because the alternative
// is a rename whose result depends on the order the shell handed the paths in.
// An operand whose name the rule leaves alone is left alone and not reported.
//
// What a rename inside one folder cannot do is cross a filesystem or land
// inside itself, so the refusals `cmdMove` makes for those are not made here.
int cmdRenameSet(const Globals &g, Session &s, const Parsed &parsed)
{
    if (parsed.replaceGiven != parsed.withGiven) {
        fail(QStringLiteral("mv: --replace and --with go together; --with '' deletes "
                            "the text"));
        return kUsage;
    }
    if (parsed.replaceGiven && parsed.replace.isEmpty()) {
        fail(QStringLiteral("mv: --replace needs some text to find"));
        return kUsage;
    }
    static const QRegularExpression token(QStringLiteral("\\{([^{}]*)\\}"));
    if (parsed.patternGiven) {
        if (parsed.pattern.isEmpty()) {
            fail(QStringLiteral("mv: --pattern needs a template"));
            return kUsage;
        }
        auto found = token.globalMatch(parsed.pattern);
        while (found.hasNext()) {
            const QString name = found.next().captured(1);
            if (name != QLatin1String("name") && name != QLatin1String("stem")
                && name != QLatin1String("ext") && name != QLatin1String("n")) {
                fail(QStringLiteral("mv: --pattern: unknown token {%1}; the tokens are "
                                    "{name}, {stem}, {ext} and {n}")
                         .arg(name));
                return kUsage;
            }
        }
    }
    if (parsed.positionals.isEmpty()) {
        fail(QStringLiteral("mv: path required"));
        return kUsage;
    }

    int status = kOk;
    // As named, link and all, the way `mv <old> <new>` takes its source.
    const QStringList sources = resolveNamed(parsed.positionals, &status);
    if (status != kOk)
        return status;

    struct Plan {
        QString from;
        QString to;
    };
    QVector<Plan> plans;
    QSet<QString> claimed;
    const int width = QString::number(sources.size()).size();
    for (int i = 0; i < sources.size(); ++i) {
        const QString &from = sources.at(i);
        const QFileInfo info(from);
        const QString was = info.fileName();
        QString name = was;
        if (parsed.replaceGiven)
            name.replace(parsed.replace, parsed.with);
        if (parsed.patternGiven) {
            // A leading dot is a hidden file and not an extension, the way
            // every shell reads `.bashrc`.
            const int dot = name.lastIndexOf(QLatin1Char('.'));
            const QString stem = dot > 0 ? name.left(dot) : name;
            const QString ext = dot > 0 ? name.mid(dot) : QString();
            // One pass, so a stem holding `{n}` is put in as the text it is.
            QString built;
            int at = 0;
            auto found = token.globalMatch(parsed.pattern);
            while (found.hasNext()) {
                const QRegularExpressionMatch hit = found.next();
                built += parsed.pattern.mid(at, hit.capturedStart() - at);
                const QString which = hit.captured(1);
                built += which == QLatin1String("name") ? name
                       : which == QLatin1String("stem") ? stem
                       : which == QLatin1String("ext")  ? ext
                       : QString::number(i + 1).rightJustified(width, QLatin1Char('0'));
                at = hit.capturedEnd();
            }
            built += parsed.pattern.mid(at);
            name = built;
        }
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
            || name.contains(QLatin1Char('/'))) {
            fail(QStringLiteral("mv: %1 would become `%2`, which is not a name")
                     .arg(from, name));
            return kUsage;
        }
        if (name == was)
            continue;
        const QString to = info.absolutePath() + QLatin1Char('/') + name;
        if (occupied(to)) {
            fail(QStringLiteral("mv: %1 already exists").arg(to));
            return kUsage;
        }
        if (claimed.contains(to)) {
            fail(QStringLiteral("mv: two of them would both become %1").arg(to));
            return kUsage;
        }
        claimed.insert(to);
        plans.append({from, to});
    }

    if ((status = openSession(&s)) != kOk)
        return status;

    // A dry run prints the plan in the shape the run prints its result: the
    // old name and the new one, tab apart, one rename to a line -- the run
    // is about a set, and a destination on its own does not say which of
    // them it came from. Under --json one `from`, `to`, `rows` object per
    // rename, the object `mv <old> <new>` already answers in, in an array.
    QJsonArray moved;
    QVector<Record> written;
    bool refused = false;
    for (const Plan &plan : std::as_const(plans)) {
        if (g.dryRun) {
            QVector<Record> rows;
            for (const QString &row : rowsUnder(s.store, plan.from)) {
                Record record = s.store.get(row);
                record.path = movedPath(row, plan.from, plan.to);
                rows.append(record);
            }
            if (g.json)
                moved.append(movedJson(plan.from, plan.to, rows));
            else
                out() << plan.from << '\t' << plan.to << '\n';
            continue;
        }
        QString why;
        bool copied = false;
        if (!moveNamed(plan.from, plan.to, &why, &copied)) {
            // The rest of the set is still tried, and every row of what did
            // move is still rewritten below: a rename that failed halfway has
            // still renamed things, and an index left behind the disk is the
            // state `verify` exists to shout about.
            fail(QStringLiteral("mv: cannot move %1 to %2: %3").arg(plan.from, plan.to, why));
            refused = true;
            continue;
        }
        QVector<Record> rows;
        followMove(g, s, plan.from, plan.to, &rows, copied);
        written += rows;
        if (g.json)
            moved.append(movedJson(plan.from, plan.to, rows));
        else
            out() << plan.from << '\t' << plan.to << '\n';
    }
    if (g.dryRun) {
        if (g.json)
            printJson(moved);
        return kOk;
    }
    const int done = finish(g, s, written, false);
    if (done != kOk)
        return done;
    if (g.json)
        printJson(moved);
    return refused ? kUsage : kOk;
}

int dispatch(const Globals &g, Session &s, QStringList rest)
{
    const QString cmd = rest.takeFirst();

    // Decided here, once, from the verb -- not guessed inside each command.
    // `tag ls` is the one subcommand of a writing verb that only reads.
    if (!s.opened) {
        const bool tagLs = cmd == QLatin1String("tag")
            && rest.value(0) == QLatin1String("ls");
        // Two of the four `roots` subcommands touch the index: `rm` carries
        // the rows out from under the declaration it removes, and `migrate`
        // rewrites them. Taking the lock for the other two would make listing
        // your own declarations wait on somebody else's `tag add`.
        const bool viewOnly = cmd == QLatin1String("view");
        const bool rootsReads = cmd == QLatin1String("roots")
            && rest.value(0) != QLatin1String("migrate")
            && rest.value(0) != QLatin1String("rm");
        // A dry run commits nothing and writes no xattr, so it wants the lock
        // no more than `ls` does -- and a preview that cannot run where a read
        // can is a preview nobody can use on a read-only index.
        s.writes = !onlyReads(cmd) && !tagLs && !rootsReads && !viewOnly && !g.dryRun;
    }
    if (cmd == QLatin1String("tag")) {
        if (rest.isEmpty()) {
            fail(QStringLiteral("tag: subcommand required "
                            "(add, rm, ls, clear, cp, merge)"));
            return kUsage;
        }
        const QString sub = rest.takeFirst();
        // Each subcommand is gated on the flags it has, the way every top-level
        // verb below is, and `tagTakes` is where it says which. One parse for
        // the whole family would let each of the six take its siblings' flags
        // and then ignore them -- `tag clear --tag drop` dropping every tag on
        // the path at exit 0 -- and `--from` eats the next word, so it would
        // swallow the `--` that separates flags from paths.
        if (sub == QLatin1String("add")) {
            Parsed parsed;
            const int parsedStatus = parseFlags(rest, &parsed, tagTakes(sub));
            if (parsedStatus != kOk)
                return parsedStatus;
            return cmdTagAdd(g, s, parsed.tags, parsed.positionals);
        }
        if (sub == QLatin1String("rm")) {
            Parsed parsed;
            const int parsedStatus = parseFlags(rest, &parsed, tagTakes(sub));
            if (parsedStatus != kOk)
                return parsedStatus;
            return cmdTagRm(g, s, parsed.tags, parsed.positionals);
        }
        if (sub == QLatin1String("ls")) {
            Parsed parsed;
            const int parsedStatus = parseFlags(rest, &parsed, tagTakes(sub));
            if (parsedStatus != kOk)
                return parsedStatus;
            return cmdTagLs(g, s, parsed.positionals);
        }
        if (sub == QLatin1String("clear")) {
            Parsed parsed;
            const int parsedStatus = parseFlags(rest, &parsed, tagTakes(sub));
            if (parsedStatus != kOk)
                return parsedStatus;
            return cmdTagClear(g, s, parsed.positionals);
        }
        if (sub == QLatin1String("cp")) {
            Parsed parsed;
            const int parsedStatus = parseFlags(rest, &parsed, tagTakes(sub));
            if (parsedStatus != kOk)
                return parsedStatus;
            return cmdTagCp(g, s, parsed.from, parsed.positionals);
        }
        if (sub == QLatin1String("merge") || sub == QLatin1String("rename")) {
            Parsed parsed;
            const int parsedStatus = parseFlags(rest, &parsed, tagTakes(sub));
            if (parsedStatus != kOk)
                return parsedStatus;
            return cmdTagMerge(g, s, parsed.positionals);
        }
        fail(QStringLiteral("tag: unknown subcommand %1").arg(sub));
        return kUsage;
    }
    if (cmd == QLatin1String("ls")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.tag = true, .lsFlags = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        QString error;
        QStringList ands;
        if (!parsed.tags.isEmpty()) {
            ands = normalizeTags(parsed.tags, &error);
            if (ands.isEmpty()) {
                fail(error);
                return kUsage;
            }
        }
        QStringList anys;
        if (!parsed.anys.isEmpty()) {
            anys = normalizeTags(parsed.anys, &error);
            if (anys.isEmpty()) {
                fail(error);
                return kUsage;
            }
        }
        qint64 sinceSeconds = -1;
        if (parsed.sinceGiven) {
            sinceSeconds = parseDuration(parsed.since, &error);
            if (sinceSeconds < 0) {
                fail(error);
                return kUsage;
            }
        }
        return cmdLs(g, s, ands, anys, parsed.untagged, parsed.cached, parsed.exts,
                     sinceSeconds, parsed.positionals);
    }
    if (cmd == QLatin1String("tags")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.tag = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        QStringList scope;
        if (!parsed.tags.isEmpty()) {
            QString error;
            scope = normalizeTags(parsed.tags, &error);
            if (scope.isEmpty()) {
                fail(error);
                return kUsage;
            }
        }
        return cmdTags(g, s, scope, parsed.positionals);
    }
    if (cmd == QLatin1String("prune")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.all = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdPrune(g, s, parsed.all, parsed.positionals);
    }
    if (cmd == QLatin1String("open")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdOpen(g, parsed.positionals);
    }
    if (cmd == QLatin1String("has")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.tag = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdHas(g, s, parsed.tags, parsed.positionals);
    }
    if (cmd == QLatin1String("json")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdJson(g, s, parsed.positionals);
    }
    if (cmd == QLatin1String("export")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        if (!parsed.positionals.isEmpty()) {
            fail(QStringLiteral("export: takes no arguments"));
            return kUsage;
        }
        return cmdExport(g, s);
    }
    if (cmd == QLatin1String("import")) {
        // Here `-` names stdin and not the start of a flag, and `parseFlags`
        // cannot tell the two apart. Same reason, same shape as `batch` below.
        if (rest.size() == 1 && rest.first() == QLatin1String("-"))
            return cmdImport(g, s, rest);
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdImport(g, s, parsed.positionals);
    }
    if (cmd == QLatin1String("mv")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.rename = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        if (parsed.replaceGiven || parsed.withGiven || parsed.patternGiven)
            return cmdRenameSet(g, s, parsed);
        return cmdMove(g, s, parsed.positionals);
    }
    if (cmd == QLatin1String("cp")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdCopy(g, s, parsed.positionals);
    }
    if (cmd == QLatin1String("trash") || cmd == QLatin1String("rm")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdRemove(g, s, parsed.positionals, cmd == QLatin1String("trash"));
    }
    if (cmd == QLatin1String("reindex")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.fromXattr = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdReindex(g, s, parsed.fromXattr, parsed.positionals);
    }
    if (cmd == QLatin1String("sweep")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.status = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdSweep(g, s, parsed.status, parsed.positionals);
    }
    if (cmd == QLatin1String("view")) {
        QString sub;
        if (!rest.isEmpty() && !rest.first().startsWith(QLatin1Char('-')))
            sub = rest.takeFirst();
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.tag = true, .view = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        QStringList tags;
        if (!parsed.tags.isEmpty()) {
            QString error;
            tags = normalizeTags(parsed.tags, &error);
            if (tags.isEmpty()) {
                fail(error);
                return kUsage;
            }
        }
        return cmdView(g, s, sub, parsed.name, tags, parsed.positionals);
    }
    if (cmd == QLatin1String("roots")) {
        QString sub;
        if (!rest.isEmpty() && !rest.first().startsWith(QLatin1Char('-')))
            sub = rest.takeFirst();
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {.root = true});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdRoots(g, s, sub, parsed.id, parsed.mount, parsed.check, parsed.positionals);
    }
    if (cmd == QLatin1String("verify")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdVerify(g, s, parsed.positionals);
    }
    if (cmd == QLatin1String("batch")) {
        // Here `-` is the name of stdin and not the start of a flag, and
        // parseFlags has no way to tell the two apart: it would refuse it as an
        // unknown flag and the documented way to pipe a file in would not work.
        if (rest.size() == 1 && rest.first() == QLatin1String("-"))
            return cmdBatch(g, s, rest);
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdBatch(g, s, parsed.positionals);
    }

    if (cmd == QLatin1String("recent")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdRecent(g, s, parsed.positionals);
    }
    if (cmd == QLatin1String("completions")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, {});
        if (parsedStatus != kOk)
            return parsedStatus;
        return cmdCompletions(g, parsed.positionals);
    }
    if (cmd == QLatin1String("__complete")) {
        // Everything after `--` is the shell's words, verbatim, including a
        // trailing empty one when the cursor sits on a fresh argument.
        QStringList words = rest;
        if (!words.isEmpty() && words.first() == QLatin1String("--"))
            words.removeFirst();
        return cmdComplete(words);
    }

    // Not one of the verbs above.
    fail(QStringLiteral("unknown command: %1 (try omafiles --help)").arg(cmd));
    return kUsage;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("omafiles"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OMAFILES_VERSION));

    Globals g;
    g.json = envFlag("OMAFILES_JSON");
    g.dryRun = envFlag("OMAFILES_DRY_RUN");
    g.yes = envFlag("OMAFILES_YES");

    QStringList rest;
    const QStringList args = QCoreApplication::arguments().mid(1);
    bool afterDash = false;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (afterDash) {
            rest.append(arg);
            continue;
        }
        if (arg == QLatin1String("--")) {
            afterDash = true;
            rest.append(arg);
            continue;
        }
        // Before the globals are read, not after: the word after an option that
        // takes one is that option's value, and a value is not a flag however
        // it is spelled. See `takesValue`. It goes through untouched, so the
        // verb's own parse binds it to the same option this pass did.
        if (takesValue(arg)) {
            rest.append(arg);
            if (i + 1 < args.size())
                rest.append(args.at(++i));
            continue;
        }
        if (isGlobalFlag(arg)) {
            applyGlobal(&g, arg);
            continue;
        }
        rest.append(arg);
    }

    if (g.version) {
        out() << "omafiles " OMAFILES_VERSION "\n";
        return finished(QString(), kOk);
    }
    // Asking for help is not a usage error. Running with no verb at all is:
    // the declaration says a subcommand is required, and a shell script that
    // reached here by accident has to be able to tell the two apart.
    if (g.help) {
        // About the verb that was named, when one was.
        const QString about = spokenVerb(rest);
        // A word that is not a verb is a mistake whether or not `--help` came
        // with it: printing the whole screen and exiting 0 is the program
        // agreeing with a typo.
        if (!about.isEmpty() && !printHelp(about)) {
            // What was asked about, whole: reporting only the first word says
            // `unknown command: tag`, naming a command that exists and hiding
            // the word that did not.
            fail(QStringLiteral("unknown command: %1 (try omafiles --help)")
                     .arg(about));
            return finished(about, kUsage);
        }
        if (about.isEmpty())
            printHelp();
        return finished(about, kOk);
    }
    if (rest.isEmpty()) {
        printHelp();
        return finished(QString(), kUsage);
    }

    // Before any verb runs, because every answer about a path depends on it:
    // whether an index row can be placed, and whether a file that is not there
    // is gone or on a disk in a drawer. A declarations file that will not parse
    // is exit 3, the way an index that will not parse is -- guessing at it
    // would be guessing at which rows may be dropped.
    QString rootsError;
    if (!loadRoots(&rootsError)) {
        fail(rootsError);
        return finished(spokenVerb(rest), kStore);
    }

    Session session;
    return finished(spokenVerb(rest), dispatch(g, session, rest));
}
