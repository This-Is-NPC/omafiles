#include "Glob.h"
#include "Names.h"
#include "Open.h"
#include "Paths.h"
#include "Recent.h"
#include "Roots.h"
#include "View.h"
#include "Store.h"
#include "Tag.h"
#include "Xattr.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <functional>
#include <cmath>
#include <cerrno>
#include <limits>
#include <optional>
#include <cstdio>
#include <cstring>
#include <functional>

#include <fcntl.h>
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

// Whether fd 0 came closed, so a name that reaches it reads nothing that was sent.
bool stdinClosed = false;

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

// git's `git_env_bool`: a value outside the two vocabularies stops the run
// rather than reading as off, because `OMAFILES_DRY_RUN=ture` must not write.
bool envFlag(const char *name, bool *on, QString *error)
{
    const QByteArray value = qgetenv(name).toLower();
    static const QList<QByteArray> yes = {"1", "true", "yes", "on"};
    static const QList<QByteArray> no = {"", "0", "false", "no", "off"};
    *on = yes.contains(value);
    if (*on || no.contains(value))
        return true;
    *error = QStringLiteral("%1=%2 is neither on (1, true, yes, on) nor off (0, false, no, "
                            "off, empty)")
                 .arg(QLatin1String(name), spelled(surrogateEscaped(qgetenv(name))));
    return false;
}

// The globals, each in both spellings.
constexpr std::pair<const char *, const char *> kGlobals[] = {
    {"-j", "--json"}, {"-n", "--dry-run"}, {"-y", "--yes"},
    {"-q", "--quiet"}, {"-h", "--help"},   {"-V", "--version"}};

// The long spelling of a global, or empty where `arg` is not one.
QString globalFlag(const QString &arg)
{
    for (const auto &[brief, spelled] : kGlobals) {
        if (arg == QLatin1String(brief) || arg == QLatin1String(spelled))
            return QString::fromLatin1(spelled);
    }
    return {};
}

bool isGlobalFlag(const QString &arg)
{
    return !globalFlag(arg).isEmpty();
}

// What an operand is, so completion offers what the verb takes there.
enum class Operand {
    Path,
    Tag,
    Shell,
    /// Typed, never offered: a mode, a count.
    Word,
};

// Which flags a verb has, and its operands. A flag accepted where it means
// nothing is a flag silently ignored, so `ls --from-xattr` is a typo rather than
// a no-op. `parseFlags` is gated on it and completion offers from it, so a flag
// can only be suggested where it will be taken.
struct Takes {
    bool tag = false;
    bool lsFlags = false;
    bool from = false;
    bool fromXattr = false;
    bool all = false;
    /// `--name`: the view's, or the name `ls` asks after.
    bool name = false;
    bool id = false;
    /// `--mount` and `--check`: where a root is.
    bool placement = false;
    bool status = false;
    /// `--replace`, `--with` and `--pattern`: `mv` over a set, by rule.
    bool rename = false;
    /// `--desktop`: `recent` answered from the desktop's list, not the index.
    bool desktop = false;
    /// `--tag` holds one value here, not a list.
    bool oneTag = false;
    /// Why `--json` means nothing here, where it does not.
    const char *noJson = nullptr;
    /// How many operands follow the flags; -1 is any number, left to the verb.
    int least = 0;
    int most = -1;
    Operand first = Operand::Path;
    Operand rest = Operand::Path;
    /// The refusal when the operands number outside `least` and `most`.
    const char *count = nullptr;
    /// The flag holding one value that the verb cannot go without.
    const char *required = nullptr;
};

// What the word after a flag is.
enum class Value {
    None,
    Path,
    Tag,
    /// `--type`'s `dir` or `file`.
    Kind,
    Word,
};

// Every flag a verb can take, in both spellings, and which `Takes` field gates
// it -- the registry cobra and clap build their parse and completion from.
struct Flag {
    QLatin1String name;
    QLatin1String brief;
    Value value;
    bool Takes::*gate;
    /// Given more than once, each adds one.
    bool list = false;
};
const Flag kFlags[] = {
    {QLatin1String("--tag"), QLatin1String("-t"), Value::Tag, &Takes::tag, true},
    {QLatin1String("--any"), {}, Value::Tag, &Takes::lsFlags, true},
    {QLatin1String("--untagged"), {}, Value::None, &Takes::lsFlags},
    {QLatin1String("--cached"), {}, Value::None, &Takes::lsFlags},
    {QLatin1String("--ext"), {}, Value::Word, &Takes::lsFlags, true},
    {QLatin1String("--name"), {}, Value::Word, &Takes::name},
    {QLatin1String("--since"), {}, Value::Word, &Takes::lsFlags},
    {QLatin1String("--type"), {}, Value::Kind, &Takes::lsFlags},
    {QLatin1String("--changed-within"), {}, Value::Word, &Takes::lsFlags},
    {QLatin1String("--from"), {}, Value::Path, &Takes::from},
    {QLatin1String("--from-xattr"), {}, Value::None, &Takes::fromXattr},
    {QLatin1String("--all"), {}, Value::None, &Takes::all},
    {QLatin1String("--id"), {}, Value::Word, &Takes::id},
    {QLatin1String("--mount"), {}, Value::None, &Takes::placement},
    {QLatin1String("--check"), {}, Value::Word, &Takes::placement},
    {QLatin1String("--status"), {}, Value::None, &Takes::status},
    {QLatin1String("--replace"), {}, Value::Word, &Takes::rename},
    {QLatin1String("--with"), {}, Value::Word, &Takes::rename},
    {QLatin1String("--pattern"), {}, Value::Word, &Takes::rename},
    {QLatin1String("--desktop"), {}, Value::None, &Takes::desktop},
};

// The flag `arg` spells, in either spelling, or null.
const Flag *flagNamed(const QString &arg)
{
    for (const Flag &flag : kFlags) {
        if (arg == flag.name || (!flag.brief.isEmpty() && arg == flag.brief))
            return &flag;
    }
    return nullptr;
}

// `--yes=1`: a flag that takes no value, handed one, said as getopt_long says
// it. Empty for any other word.
QString valueRefused(const QString &arg)
{
    const qsizetype equals = arg.indexOf(QLatin1Char('='));
    if (!arg.startsWith(QLatin1String("--")) || equals < 3)
        return {};
    const QString name = arg.left(equals);
    const Flag *flag = flagNamed(name);
    if (!isGlobalFlag(name) && (!flag || flag->value != Value::None))
        return {};
    return QStringLiteral("%1 takes no value, and %2 gives it one").arg(name, spelled(arg));
}

// Which flags take the next word as their value, anywhere in the program.
//
// POSIX Utility Syntax Guideline 6: the token following an option that takes an
// argument is that argument, taken literally. The globals are stripped in a
// pass before the verb's own parse, so without this `ls --ext -j sub` reads
// `-j` as `--json` and `--ext` eats the `[dir]`. Every verb's, since that pass
// does not know the verb yet.
bool takesValue(const QString &arg)
{
    const Flag *flag = flagNamed(arg);
    return flag && flag->value != Value::None;
}

// Whether `flag` may be given more than once here, each adding one.
bool repeats(const Flag &flag, const Takes &takes)
{
    return flag.list && !(takes.oneTag && flag.name == QLatin1String("--tag"));
}

// The verbs that take a subcommand, and theirs. `rename` is the alias of
// `tag merge`; `view` and `roots` alone mean `ls`.
QStringList subcommandsOf(const QString &verb)
{
    if (verb == QLatin1String("tag"))
        return {QStringLiteral("add"),   QStringLiteral("rm"), QStringLiteral("ls"),
                QStringLiteral("clear"), QStringLiteral("cp"), QStringLiteral("merge"),
                QStringLiteral("rename")};
    if (verb == QLatin1String("view"))
        return {QStringLiteral("add"), QStringLiteral("ls"), QStringLiteral("rm")};
    if (verb == QLatin1String("roots"))
        return {QStringLiteral("add"), QStringLiteral("ls"), QStringLiteral("migrate"),
                QStringLiteral("rm")};
    return {};
}

// Every verb's flags and operands, and each subcommand's own.
Takes takesFor(const QString &verb, const QString &sub = {})
{
    // A `[dir]`, which `verify`, `reindex`, `prune`, `sweep`, `ls` and `tags` take.
    constexpr const char *oneDir = "expected at most one directory";
    if (verb == QLatin1String("tag")) {
        if (sub == QLatin1String("add") || sub == QLatin1String("rm"))
            return {.tag = true, .least = 1, .count = "path required", .required = "--tag"};
        if (sub == QLatin1String("cp"))
            return {.from = true, .least = 1, .count = "path required", .required = "--from"};
        if (sub == QLatin1String("clear"))
            return {.least = 1, .count = "path required"};
        if (sub == QLatin1String("ls"))
            return {.least = 1, .most = 1, .count = "one path required"};
        if (sub == QLatin1String("merge") || sub == QLatin1String("rename"))
            return {.least = 2, .most = 2, .first = Operand::Tag, .rest = Operand::Tag,
                    .count = "takes exactly two tags, <from> and <to>"};
        return {};
    }
    if (verb == QLatin1String("view")) {
        if (sub == QLatin1String("add"))
            return {.tag = true, .name = true, .most = 1, .required = "--name"};
        if (sub == QLatin1String("rm"))
            return {.name = true, .most = 0, .required = "--name"};
        return {.most = 0};                      // ls, or none
    }
    if (verb == QLatin1String("roots")) {
        if (sub == QLatin1String("add"))
            return {.id = true, .placement = true, .least = 1, .most = 1, .required = "--id"};
        if (sub == QLatin1String("rm"))
            return {.id = true, .most = 0, .required = "--id"};
        return {.most = 0};                      // ls, migrate, or none
    }
    if (verb == QLatin1String("ls"))
        return {.tag = true, .lsFlags = true, .name = true, .most = 1, .count = oneDir};
    if (verb == QLatin1String("tags"))
        return {.tag = true, .most = 1, .count = oneDir};
    if (verb == QLatin1String("has"))
        return {.tag = true, .oneTag = true, .least = 1, .most = 1,
                .count = "one path required", .required = "--tag"};
    if (verb == QLatin1String("json"))
        return {.least = 1, .most = 1, .count = "one path required"};
    if (verb == QLatin1String("open"))
        return {.least = 1, .count = "path required"};
    if (verb == QLatin1String("prune"))
        return {.all = true, .most = 1, .count = oneDir};
    if (verb == QLatin1String("reindex"))
        return {.fromXattr = true, .most = 1, .count = oneDir};
    if (verb == QLatin1String("sweep"))
        return {.status = true, .most = 1, .count = oneDir};
    if (verb == QLatin1String("verify"))
        return {.most = 1, .count = oneDir};
    // Two paths, where a rename flag does not make it a set: see `renamesSet`.
    if (verb == QLatin1String("mv") || verb == QLatin1String("cp"))
        return {.rename = verb == QLatin1String("mv"), .least = 2, .most = 2,
                .count = "takes exactly two paths, <old> and <new>"};
    if (verb == QLatin1String("rm") || verb == QLatin1String("trash"))
        return {.least = 1, .count = "path required"};
    if (verb == QLatin1String("chmod"))
        return {.least = 2, .first = Operand::Word,
                .count = "a mode and at least one path required"};
    if (verb == QLatin1String("batch"))
        return {.most = 1, .count = "one file at most"};
    if (verb == QLatin1String("recent"))
        return {.desktop = true, .most = 1, .first = Operand::Word, .count = "one count at most"};
    if (verb == QLatin1String("import"))
        return {.least = 1, .most = 1, .count = "one file required"};
    if (verb == QLatin1String("export"))
        return {.most = 0};
    if (verb == QLatin1String("completions"))
        return {.noJson = "this prints a shell script, which has no JSON shape",
                .least = 1, .most = 1, .first = Operand::Shell,
                .count = "one shell required (bash, zsh, fish)"};
    return {};
}

// Whether `given` holds a rename flag, which makes `mv` a set renamed by rule.
bool renamesSet(const QSet<QString> &given)
{
    return std::any_of(std::begin(kFlags), std::end(kFlags), [&given](const Flag &flag) {
        return flag.gate == &Takes::rename && given.contains(flag.name);
    });
}

// `takes` once `given` is on the line: a set renamed by rule is one path or more.
Takes bySet(Takes takes, const QSet<QString> &given)
{
    if (renamesSet(given)) {
        takes.least = 1;
        takes.most = -1;
        takes.count = "path required";
    }
    return takes;
}

// Two things one verb refuses together, and why: the parse refuses from it and
// completion declines to offer from it, as clap_complete reads `conflicts_with`.
// An empty `other` is an operand.
struct Conflict {
    QLatin1String verb;
    QLatin1String flag;
    QLatin1String other;
    QLatin1String why;
};
constexpr QLatin1String kNoTag("ask opposite questions; a file with no tags matches no tag");
constexpr QLatin1String kCachedWhy(
    "ask different things; --cached answers --untagged and --name without walking the "
    "disk, and a tag is asked of the index, which is already read whole");
const Conflict kConflicts[] = {
    {QLatin1String("ls"), QLatin1String("--untagged"), QLatin1String("--tag"), kNoTag},
    {QLatin1String("ls"), QLatin1String("--untagged"), QLatin1String("--any"), kNoTag},
    {QLatin1String("ls"), QLatin1String("--untagged"), QLatin1String("--since"),
     QLatin1String("ask opposite questions; an untagged file was never tagged")},
    {QLatin1String("ls"), QLatin1String("--cached"), QLatin1String("--tag"), kCachedWhy},
    {QLatin1String("ls"), QLatin1String("--cached"), QLatin1String("--any"), kCachedWhy},
    {QLatin1String("ls"), QLatin1String("--cached"), QLatin1String("--since"), kCachedWhy},
    {QLatin1String("prune"), QLatin1String("--all"), {},
     QLatin1String("--all is the whole index; it takes no directory")},
    {QLatin1String("sweep"), QLatin1String("--status"), {},
     QLatin1String("--status reports the last run; it takes no directory")},
    {QLatin1String("view add"), QLatin1String("--tag"), {},
     QLatin1String("a view is a folder or a set of tags, not both; the window has no way to "
                   "walk a tag query inside a folder")},
};

// A flag that answers another and means nothing without one of them.
struct Needs {
    QLatin1String verb;
    QLatin1String flag;
    QLatin1String oneOf[2];
    QLatin1String why;
};
const Needs kNeeds[] = {
    {QLatin1String("ls"), QLatin1String("--cached"),
     {QLatin1String("--untagged"), QLatin1String("--name")},
     QLatin1String("--cached answers --untagged and --name without walking the disk; every "
                   "other listing is the index, which is already read whole")},
};

// The conflict `flag` is in with a flag of `given`, or null.
const Conflict *pairedConflict(const QString &named, const QString &flag,
                               const QSet<QString> &given)
{
    for (const Conflict &conflict : kConflicts) {
        if (conflict.verb == named && !conflict.other.isEmpty()
            && ((flag == conflict.flag && given.contains(conflict.other))
                || (flag == conflict.other && given.contains(conflict.flag))))
            return &conflict;
    }
    return nullptr;
}

// The refusal `flag` earns beside `given`, the other flags on a line, for
// `named` out of `kConflicts` and `kNeeds`, or empty. `operands` is whether
// one was given.
QString conflictOf(const QString &named, const QString &flag, const QSet<QString> &given,
                   bool operands)
{
    for (const Conflict &conflict : kConflicts) {
        if (conflict.verb == named && conflict.other.isEmpty() && flag == conflict.flag
            && operands)
            return QStringLiteral("%1: %2").arg(named, conflict.why);
    }
    if (const Conflict *conflict = pairedConflict(named, flag, given))
        return QStringLiteral("%1: %2 and %3 %4")
            .arg(named, conflict->flag, conflict->other, conflict->why);
    for (const Needs &needs : kNeeds) {
        if (needs.verb == named && flag == needs.flag
            && std::none_of(std::begin(needs.oneOf), std::end(needs.oneOf),
                            [&given](QLatin1String other) { return given.contains(other); }))
            return QStringLiteral("%1: %2").arg(named, needs.why);
    }
    return {};
}

// Where the `=` is in `--flag=value`, for the long flags that take a value --
// GNU `getopt_long`'s spelling, which git, clap and argparse share -- or -1.
int attachedAt(const QString &word)
{
    const int equals = word.indexOf(QLatin1Char('='));
    return word.startsWith(QLatin1String("--")) && equals > 2 && takesValue(word.left(equals))
        ? equals
        : -1;
}

// `--flag=value` as the two words it means. Never past `--`, and never inside
// a value.
QStringList attachedSplit(const QStringList &args)
{
    QStringList words;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--")) {
            words.append(args.mid(i));
            break;
        }
        const int equals = attachedAt(arg);
        if (takesValue(arg)) {
            words.append(arg);
            if (i + 1 < args.size())
                words.append(args.at(++i));
        } else if (equals > 0) {
            words << arg.left(equals) << arg.mid(equals + 1);
        } else {
            words.append(arg);
        }
    }
    return words;
}

// `args` with the globals taken out into `said`, as the run and completion
// both read them: none past `--`, and none that is a flag's value.
//
// Before the globals are read, not after: the word after an option that takes
// one is that option's value, and a value is not a flag however it is spelled.
// See `takesValue`. It goes through untouched, so the verb's own parse binds it
// to the same option this pass did.
QStringList withoutGlobals(const QStringList &args, QStringList *said)
{
    QStringList rest;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--")) {
            rest.append(args.mid(i));
            break;
        }
        if (takesValue(arg)) {
            rest.append(arg);
            if (i + 1 < args.size())
                rest.append(args.at(++i));
        } else if (isGlobalFlag(arg)) {
            said->append(arg);
        } else {
            rest.append(arg);
        }
    }
    return rest;
}

void applyGlobal(Globals *g, const QString &arg)
{
    const QString flag = globalFlag(arg);
    if (flag == QLatin1String("--json")) {
        g->json = true;
        g->jsonAsked = true;
    } else if (flag == QLatin1String("--dry-run"))
        g->dryRun = true;
    else if (flag == QLatin1String("--yes"))
        g->yes = true;
    else if (flag == QLatin1String("--quiet"))
        g->quiet = true;
    else if (flag == QLatin1String("--help"))
        g->help = true;
    else if (flag == QLatin1String("--version"))
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
        if (subcommandsOf(verb).isEmpty())
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
// Named by the verb that was writing, or the program where none was, like every
// other diagnostic here and like coreutils' own `ls: write error`.
int finished(const QString &verb, int status)
{
    out().flush();
    // Both halves: QTextStream writes into stdio, and stdio writes to the fd,
    // so a full disk can surface at either one. The reason is the one taken at
    // the failing call -- QFile keeps it -- because errno by now is anybody's.
    QString reason;
    if (out().status() != QTextStream::Ok)
        reason = out().device()->errorString();
    else if (std::fflush(stdout) != 0)
        reason = systemReason(errno);
    if (reason.isEmpty())
        return status;
    fail(QStringLiteral("%1: write error: %2")
             .arg(verb.isEmpty() ? QStringLiteral("omafiles") : verb, reason));
    return status == kOk ? 1 : status;
}

void printJson(const QJsonValue &value)
{
    const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray())
                                               : QJsonDocument(value.toObject());
    out() << QString::fromUtf8(doc.toJson(QJsonDocument::Compact)) << '\n';
}

QJsonArray rowsJson(const QVector<Record> &rows)
{
    QJsonArray array;
    for (const Record &record : rows)
        array.append(record.toJson());
    return array;
}

// A listing's answer: one array of rows under `--json`, a path a line without.
void printRows(const Globals &g, const QVector<Record> &rows)
{
    if (g.json) {
        printJson(rowsJson(rows));
        return;
    }
    for (const Record &record : rows)
        out() << record.path << '\n';
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
  ls       [--tag <tag>]... [--any <tag>]... [--untagged] [--name <glob>]
           [--cached] [--ext <ext>]... [--type file|dir]
           [--changed-within <duration>] [--since <duration>] [dir]
                                    --since counts from when it was tagged,
                                    --changed-within from the file's mtime
                                    --name alone asks the disk, like
                                    --untagged; with a tag it narrows the rows
                                    --cached answers --untagged and --name
                                    out of the list sweep built, and says
                                    its age
  tags     [--tag <tag>]... [dir]   one line per tag, with its count;
                                    --tag counts what appears beside it
  has      --tag <tag> -- <path>    exit 0 if tagged, 1 if not
  json     -- <path>
  recent   [--desktop] [n]          last tagged first; --desktop lists what
                                    the desktop opened lately instead
  export                            one index row per line, as jsonl: many
                                    documents, which a JSON parser refuses
                                    --json instead dumps them as one array
  import   <file>                   merge a dump into the index
                                    - reads stdin, the way batch does

Moving:
  open     <path>...                what the desktop would use, or xdg-open
  mv       <old> <new>              file and its tags; a folder cannot
                                    cross a filesystem, and it will not
                                    write over what is already there
  mv       [--replace <text>] [--with <text>] [--pattern <template>]
           -- <path>...             rename a set by rule, each in its own
                                    folder: {name} {stem} {ext} {n}
  cp       <old> <new>              file and its tags; will not write over
                                    what is already there
  trash    <path>...                gio trash, then drop the rows; one
                                    operand it cannot take makes it
                                    refuse the whole list
  rm       <path>...                delete, then drop the rows; one
                                    operand it cannot take makes it
                                    refuse the whole list
  chmod    <mode> <path>...         rwx for owner, group and others: 644,
                                    or u+x,go-w; never setuid, setgid or
                                    sticky, never through a symlink

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
                                    or under a root declared inside it
  roots migrate                     rewrite every row in the form roots give it
                                    a row on a declared root that is not here
                                    is unreachable, not missing: prune and
                                    verify both step over it
  prune    [dir] [--all]            drop rows whose file is gone (asks first)
                                    a directory, or --all for the whole index
  reindex  [--from-xattr] [dir]     rebuild index rows from a tree
                                    exit 1 if it had to walk away from a path
  sweep    [--status] [dir]         the run you do not watch: adopt what the
                                    files carry, count what is gone, drop
                                    nothing; the declared roots by default
                                    --status prints the last run's receipt
  verify   [dir]                    exit 1 if index and disk disagree
                                    walks the current directory by default

Many at once:
  batch    [file]                   one command per line, one store write
                                    no nesting, no read verbs, no rm, mv,
                                    cp, trash, chmod, view, roots or sweep,
                                    and the globals go before it

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
    // The shape each entry's usage line carries. `ls` wraps its flags onto a
    // second line, so a synopsis is gathered from every line left of the
    // description column, not just the first.
    QStringList shapes;
    // Whether an entry names a subcommand of `verb` rather than `verb` itself.
    bool family = false;
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
            // `tag add` is spelled in two words and `tag` alone names all six;
            // `view` and `roots` the same.
            const QStringList words = trimmed.split(QLatin1Char(' '),
                                                    Qt::SkipEmptyParts);
            const QString named = subcommandsOf(words.value(0)).contains(words.value(1))
                ? words.value(0) + QLatin1Char(' ') + words.value(1)
                : words.value(0);
            // An entry names its own aliases, so the spelling the body does
            // not start with is still findable -- `tag rename` for
            // `tag merge`.
            const bool alias = trimmed.contains(QStringLiteral("(alias: %1)").arg(verb));
            taking = named == verb || words.value(0) == verb || alias;
            if (taking) {
                family = family || (named != verb && !alias);
                int start = 0;
                while (start < line.size() && line.at(start) == QLatin1Char(' '))
                    ++start;
                shapes.append(synopsisOf(line, start + named.size()));
            }
        } else if (taking && !trimmed.isEmpty()
                   && line.size() - trimmed.size() < kDescriptionColumn) {
            // A continuation left of the description column is more synopsis;
            // one at it is prose about the entry above.
            const QString more = synopsisOf(line, 0);
            if (!more.isEmpty())
                shapes.last() = (shapes.last() + QLatin1Char(' ') + more).trimmed();
        }
        if (taking && !trimmed.isEmpty())
            lines.append(line);
    }

    // Nothing to say about it, because it is not a verb. The caller decides
    // what that means.
    if (lines.isEmpty())
        return false;
    // One usage line per form of the verb, as git's `usage:` block lists them.
    // `omafiles tag --help` names six subcommands, which are not forms of `tag`;
    // each entry below carries its own.
    if (family)
        shapes = {QString()};
    for (qsizetype i = 0; i < shapes.size(); ++i) {
        out() << (i == 0 ? QStringLiteral("Usage: ") : QStringLiteral("       "))
              << QStringLiteral("omafiles ") << verb;
        if (!shapes.at(i).isEmpty())
            out() << ' ' << shapes.at(i);
        out() << '\n';
    }
    out() << '\n';
    for (const QString &line : lines)
        out() << line << '\n';
    // A verb that refuses --json must not offer the flag two lines under the
    // sentence that rejects it.
    out() << (takesFor(verb).noJson
                  ? QStringLiteral("\nGlobals: --dry-run  --yes  --quiet\n")
                  : QStringLiteral("\nGlobals: --json  --dry-run  --yes  --quiet\n"));
    return true;
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
    static const QRegularExpression re(
        QRegularExpression::anchoredPattern(QStringLiteral("(\\d+)([smhd])")));
    const QRegularExpressionMatch match = re.match(text);
    // One message: a duration nobody can represent is as unusable as one nobody
    // can parse, and the shape it should have had is the useful half of both.
    *error = QStringLiteral("invalid duration: %1 (use e.g. 7d, 12h, 30m, 90s)").arg(spelled(text));
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

// A suffix is what follows the last dot of a name, so an empty one, or one
// holding a dot, a `*` or a `/`, is a question no name can answer.
QString normalizeExt(const QString &raw, QString *error)
{
    const QString ext = raw.startsWith(QLatin1Char('.')) ? raw.mid(1) : raw;
    if (ext.isEmpty() || ext.contains(QLatin1Char('.')) || ext.contains(QLatin1Char('*'))
        || ext.contains(QLatin1Char('/'))) {
        *error = QStringLiteral("--ext takes a suffix like pdf or .pdf, not %1; it is "
                                "what follows the last dot of a name, so it holds no dot, "
                                "* or /")
                     .arg(spelled(raw));
        return {};
    }
    return ext.toLower();
}

// coreutils `same_file`: a file is its device and inode, so a hard link is
// the file it links.
using FileId = std::pair<dev_t, ino_t>;

FileId fileId(const struct stat &st)
{
    return {st.st_dev, st.st_ino};
}

// The file `path` leads to; none where the system will not say.
std::optional<FileId> fileIdOf(const QString &path)
{
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return std::nullopt;
    return fileId(st);
}

// Where the last run's receipt lives.
//
// State, and neither data nor a declaration: it records that something
// happened rather than what the index or the roots are, losing it costs only
// the answer to "when did this last run", and nothing reads it back into a
// decision. `$XDG_STATE_HOME` is the directory the basedir spec names for
// exactly that.
QString sweepReceiptPath()
{
    return QDir(stateHome()).filePath(QStringLiteral("omafiles/sweep.json"));
}

// A path a writing verb leaves alone, told by device and inode as coreutils
// `same_file` tells a file, so neither a hard link nor another mount of its
// directory is a way round.
struct Guarded {
    QString path;
    std::optional<FileId> self;
    QVector<FileId> above;      // every directory it is in that is there, nearest first
    QString entry;              // its name in the first of those, or its missing directory's
    bool entryIsSelf = true;
};

Guarded guarded(const QString &path)
{
    Guarded guard{path, std::nullopt, {}, {}};
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) == 0)
        guard.self = fileId(st);
    QString below = path;
    for (QString dir = typedDirectory(path, nullptr);;) {
        if (const std::optional<FileId> id = fileIdOf(dir)) {
            if (guard.above.isEmpty()) {
                typedDirectory(below, &guard.entry);
                guard.entryIsSelf = below == path;
            }
            guard.above.append(*id);
        } else if (guard.above.isEmpty()) {
            below = dir;
        }
        const QString up = typedDirectory(dir, nullptr);
        if (up == dir)
            return guard;
        dir = up;
    }
}

enum class Guards { No, Is, Holds };

// Whether `path`, a link not followed, is `guard` or holds it, by `found`, its
// `lstat`, where it has one; a path that is not there is `guard`, or the
// directory it will be made in, where it names the same entry of the same
// directory, by `foldedName` where that directory folds case.
Guards guards(const QString &path, const struct stat *found, const Guarded &guard)
{
    if (found) {
        if (guard.self == fileId(*found))
            return Guards::Is;
        return guard.above.contains(fileId(*found)) ? Guards::Holds : Guards::No;
    }
    QString name;
    const QString dir = typedDirectory(path, &name);
    const std::optional<FileId> parent = fileIdOf(dir);
    if (!parent || guard.above.value(0) != *parent)
        return Guards::No;
    if (name != guard.entry && (foldedName(name) != foldedName(guard.entry) || !foldsCase(dir)))
        return Guards::No;
    return guard.entryIsSelf ? Guards::Is : Guards::Holds;
}

// Names each of `paths`, resolved, that is one of `guarded`, or holds one
// where `holding` counts, into `status`, with `why` of that one and whether it
// is held.
void refuseGuarded(const QString &verb, const QStringList &paths,
                   const QVector<Guarded> &guarded,
                   const std::function<QString(const QString &, bool)> &why, int *status,
                   bool holding = true)
{
    for (const QString &path : paths) {
        struct stat st {};
        const bool there = ::lstat(QFile::encodeName(path).constData(), &st) == 0;
        for (const Guarded &guard : guarded) {
            const Guards found = guards(path, there ? &st : nullptr, guard);
            if (found == Guards::No || (found == Guards::Holds && !holding))
                continue;
            fail(QStringLiteral("%1: refusing %2: %3")
                     .arg(verb, spelled(path), why(guard.path, found == Guards::Holds)));
            if (*status == kOk)
                *status = kUsage;
            break;
        }
    }
}

// The index, its lock, the declarations, the views, the lists of names and the
// receipt: the files only the verbs that keep them write.
QVector<Guarded> keptFiles()
{
    QVector<Guarded> kept;
    for (const QString &file : {defaultIndexPath(), Store::lockPath(defaultIndexPath()),
                                defaultRootsPath(), defaultViewsPath(), sweepReceiptPath()})
        kept.append(guarded(file));
    for (const Root &root : roots())
        kept.append(guarded(namesCachePath(root.id)));
    return kept;
}

// Every other writer leaves `keptFiles` alone -- its bytes, its mode and its
// tags -- as git leaves `.git` alone. `holding` is false for a verb that
// changes the directory named and nothing in it: `chmod`, and the tag writers.
void refuseKept(const QString &verb, const QStringList &paths, int *status,
                bool holding = true, const QVector<Guarded> &kept = keptFiles())
{
    refuseGuarded(verb, paths, kept, [](const QString &file, bool holds) {
        if (!holds)
            return QStringLiteral("omafiles keeps it");
        // A kept file not made yet is still where it will be made, and so is
        // one a file stands in the way of.
        return leadsNowhere(file)
            ? QStringLiteral("it is where omafiles keeps %1").arg(spelled(file))
            : QStringLiteral("it holds %1, which omafiles keeps").arg(spelled(file));
    }, status, holding);
}

// What a walk of one tree found, read with `readDirectory` and `lstat` so it
// is what the disk holds: `find -P` lists the same entries.
struct Walk {
    QString base;            // the tree, resolved
    /// Every entry under the tree as it is named -- a link by its own name and
    /// never followed, a fifo and a socket too -- in order, the tree itself out.
    QStringList paths;
    QSet<QString> links;     // the entries of `paths` that are symlinks
    /// Directories that would not open. None is in `paths`, and nothing under
    /// one was walked.
    QStringList shut;
    /// What the system said about each of `shut`, without the path.
    QMap<QString, QString> shutWhy;
    /// Trees under a declared root that is away, each to the sentence naming the
    /// root. Whatever answers at such a path is not the storage, so none is walked.
    QMap<QString, QString> away;
    /// Entries a directory listed and the system would not describe -- one
    /// readable and not searchable -- each to `<path>: <why>`, none in `paths`.
    QMap<QString, QString> undescribed;
    /// Names no path here can spell, mangled. See `readDirectory`.
    QStringList unnamed;
    /// Trees the caller covers on its own: each is an entry, and nothing under
    /// one is walked.
    QSet<QString> apart;
    /// The `keptFiles` that are there, which a walk skips as git skips `.git`,
    /// by any name and through any link.
    QVector<FileId> kept;
};

// What the system said about `path`, without the path: every reason here is
// spelled `<path>: <reason>`, and a sentence that already names the path says
// the rest once.
QString reasonOnly(const QString &said, const QString &path)
{
    const QString prefix = spelled(path) + QLatin1String(": ");
    return said.startsWith(prefix) ? said.mid(prefix.size()) : said;
}

void walkInto(const QString &directory, Walk *walk)
{
    if (walk->apart.contains(directory)) {
        walk->paths.append(directory);
        return;
    }
    // Asked where a root can begin or end: the tree the walk starts in, and a
    // directory some root is declared at.
    QString why;
    const PathState storage = directory == walk->base || rootAt(directory)
        ? storageState(directory, &why)
        : PathState::Present;
    if (storage == PathState::Unreachable) {
        // A nested root's own directory is an entry of the tree above it.
        if (directory != walk->base)
            walk->paths.append(directory);
        walk->away.insert(directory, why);
        return;
    }
    QStringList names;
    QStringList mangled;
    PathState state = storage;
    if (storage != PathState::Present
        || !readDirectory(directory, &names, &mangled, &why, &state)) {
        // Gone between the listing above it and now is not a directory shut.
        if (state != PathState::Missing) {
            walk->shut.append(directory);
            walk->shutWhy.insert(directory, reasonOnly(why, directory));
        }
        return;
    }
    if (directory != walk->base)
        walk->paths.append(directory);
    const QString prefix = directory.endsWith(QLatin1Char('/'))
        ? directory
        : directory + QLatin1Char('/');
    for (const QString &spelled : std::as_const(mangled))
        walk->unnamed.append(prefix + spelled);
    names.sort();
    for (const QString &name : std::as_const(names)) {
        const QString path = prefix + name;
        struct stat st {};
        QString why;
        const PathState state = statState(path, &why, false, &st);
        if (state != PathState::Present) {
            // Gone since the listing is not an entry refused.
            if (state != PathState::Missing)
                walk->undescribed.insert(path, why);
            continue;
        }
        if (walk->kept.contains(fileId(st)))
            continue;
        // A declared root's path is that root whatever stands there now, so a
        // link put in its place is asked as the root is, and found away.
        if (S_ISDIR(st.st_mode) || rootAt(path)) {
            walkInto(path, walk);
            continue;
        }
        if (S_ISLNK(st.st_mode))
            walk->links.insert(path);
        walk->paths.append(path);
    }
}

// `base` is resolved already: every tree handed here came through
// `canonicalPath` or is a declared root. `apart` holds trees inside it.
Walk walkTree(const QString &base, const QSet<QString> &apart = {})
{
    Walk walk;
    walk.base = base;
    walk.apart = apart;
    for (const Guarded &file : keptFiles()) {
        if (file.self)
            walk.kept.append(*file.self);
    }
    walkInto(walk.base, &walk);
    return walk;
}

// Whether `path` belongs in `root`'s list of names: its longest-matching root,
// except that a root's own directory is an entry of the tree above it, since
// a walk never yields the directory it starts in.
bool listedUnder(const QString &path, const Root &root)
{
    const Root *own = rootAt(path) ? rootFor(typedDirectory(path, nullptr)) : rootFor(path);
    return own && own->id == root.id;
}

// A directory that would not open, said with what did not happen under it --
// `listed`, `swept` -- and what the system said.
// Named where it is shut: a directory reached through one that refuses search,
// as a working directory whose parent was chmod'd after `cd`, is that parent's.
QString shutGap(const QString &directory, const QString &done, const QString &why)
{
    QString shut = directory;
    for (struct stat st {}; ::stat(QFile::encodeName(shut).constData(), &st) != 0
                            && errno == EACCES && QFileInfo(shut).path() != shut;)
        shut = QFileInfo(shut).path();
    return QStringLiteral("%1 is unreadable; nothing under it was %2: %3")
        .arg(spelled(shut), done, why);
}

// What a walk could not cover, one sentence each, for the paths `keep` takes.
QVector<Gap> walkGaps(const Walk &walk, const QString &done,
                      const std::function<bool(const QString &)> &keep = {})
{
    const auto kept = [&keep](const QString &path) { return !keep || keep(path); };
    QVector<Gap> gaps;
    for (const QString &closed : walk.shut) {
        if (kept(closed))
            gaps.append({closed, shutGap(closed, done, walk.shutWhy.value(closed))});
    }
    // An away root under the tree whose directory is gone, as udisks removes a
    // mount point, is named from its declaration, since no walk meets it.
    QMap<QString, QString> away = walk.away;
    const auto covered = [&walk, &away](const QString &path) {
        const auto holds = [&path](const QString &tree) { return isUnderDirectory(path, tree); };
        return std::any_of(away.keyBegin(), away.keyEnd(), holds)
            || std::any_of(walk.shut.cbegin(), walk.shut.cend(), holds)
            || std::any_of(walk.apart.cbegin(), walk.apart.cend(), holds);
    };
    // By path, so a root is met before the roots nested in it.
    QVector<Root> declared = roots();
    std::sort(declared.begin(), declared.end(),
              [](const Root &a, const Root &b) { return a.path < b.path; });
    for (const Root &root : std::as_const(declared)) {
        QString why;
        if (root.path != walk.base && isUnderDirectory(root.path, walk.base)
            && !covered(root.path) && storageState(root.path, &why) == PathState::Unreachable)
            away.insert(root.path, why);
    }
    for (auto waiting = away.cbegin(); waiting != away.cend(); ++waiting) {
        if (kept(waiting.key())) {
            gaps.append({waiting.key(), QStringLiteral("%1; nothing under it was %2")
                                            .arg(waiting.value(), done)});
        }
    }
    for (auto error = walk.undescribed.cbegin(); error != walk.undescribed.cend(); ++error) {
        if (kept(error.key()))
            gaps.append({error.key(), QStringLiteral("%1; it was not %2").arg(error.value(), done)});
    }
    for (const QString &mangled : walk.unnamed) {
        if (kept(mangled)) {
            gaps.append({mangled, QStringLiteral("%1 cannot be named; the name is not valid "
                                                 "UTF-8, so neither it nor anything under it "
                                                 "was %2")
                                      .arg(mangled, done)});
        }
    }
    return gaps;
}

// The walk as the tag verbs take it: the tree itself, then every entry, with a
// link read as the file it leads to -- which is what a tag on a link means,
// since a `user.*` xattr cannot sit on one. A link leading nowhere, out of the
// tree, or into a directory the walk could not open or left apart is dropped,
// and each file comes once however many names reach it, so no row is written
// twice. A link the system will not resolve -- a loop, a target shut to us, a
// name that is not valid UTF-8 -- is not one leading nowhere: it goes into
// `unresolved`, link to reason.
QStringList tagTargets(const Walk &walk, QMap<QString, QString> *unresolved)
{
    const QSet<QString> shut(walk.shut.cbegin(), walk.shut.cend());
    QStringList found;
    QSet<QString> seen;
    const auto skipped = [&walk](const QString &path) {
        const auto holds = [&path](const QString &tree) { return isUnderDirectory(path, tree); };
        return std::any_of(walk.away.keyBegin(), walk.away.keyEnd(), holds)
            || std::any_of(walk.apart.cbegin(), walk.apart.cend(), holds);
    };
    const auto take = [&](const QString &path) {
        if (shut.contains(path) || skipped(path) || seen.contains(path))
            return;
        seen.insert(path);
        found.append(path);
    };
    take(walk.base);
    for (const QString &path : walk.paths) {
        if (!walk.links.contains(path)) {
            take(path);
            continue;
        }
        QString why;
        PathState state = PathState::Present;
        const QString target = realPath(path, &why, &state);
        if (state == PathState::Unknown && !leadsNowhere(path))
            unresolved->insert(path, why);
        if (target.isEmpty() || !isUnderDirectory(target, walk.base))
            continue;
        if (const std::optional<FileId> id = fileIdOf(target); !id || !walk.kept.contains(*id))
            take(target);
    }
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
    /// Every path a `reindex` walked: it promises the index and the files agree
    /// about that tree, so a copy of one the flush could not write is why the
    /// run exits 1. See `flushPending`.
    QSet<QString> walked;
    /// The paths a `tag clear` dropped, the one verb licensed to rewrite a value
    /// holding a word that is not a tag.
    QSet<QString> cleared;
    /// The paths whose copy the last flush wrote, or in a dry run would.
    QSet<QString> mirrored;
    /// Every file whose own copy this run writes, dry run too, by any of its
    /// names, and the path whose row that copy is: a later line reads the file's
    /// tags from that row and not from the copy the flush is about to overwrite.
    QHash<FileId, QString> settled;
    /// Whether the run walked away from something it was asked to settle. The
    /// rows it wrote stand -- this is a diagnosis and not a failed write -- so
    /// the verb exits 1 with its answer printed, and a `batch` reports it after
    /// its own commit rather than rolling back.
    ///
    /// Every message that comes with one goes through `fail` and not `warn`:
    /// `--quiet` is "No stderr except errors", and a path a run declined to
    /// settle is why it exits 1, not a note about it.
    bool incomplete = false;
    /// The verb as a diagnostic names it -- `batch` for every line of one.
    QString verb;
    /// The verb of the line that queued each pending copy, which a note the
    /// flush gives about that copy carries, as the line's own notes do.
    QString line;
    QHash<QString, QString> queuedBy;
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
// through. A path with no row now is that path with no tags, the receipt `tag
// clear` prints; `changedBy` then keeps only what the run changed.
//
// For the two verbs whose one answer covers many writes -- `batch` over its
// lines, `import` over its dump. A verb run on its own reports the row it
// wrote, emptied or not: that is its receipt and not a summary of a run.
QVector<Record> committedRows(const Store &store, const QVector<Record> &touched)
{
    QStringList order;
    QHash<QString, Record> last;
    for (const Record &record : touched) {
        if (!last.contains(record.path))
            order.append(record.path);
        last.insert(record.path, record);
    }
    QVector<Record> rows;
    for (const QString &path : std::as_const(order)) {
        // Emptied, it is the write that emptied it, stamped with the drop.
        Record kept = store.get(path);
        if (kept.path.isEmpty()) {
            kept = last.value(path);
            kept.tags.clear();
        }
        rows.append(kept);
    }
    return rows;
}

QStringList salvagedTags(const QStringList &raw, QStringList *rejected);

// Words out of a value, as a message names them: each once, escaped.
QString saidWords(const QStringList &words)
{
    QStringList said;
    for (const QString &word : words) {
        const QString one = word.isEmpty() ? QStringLiteral("an empty word") : escapedWords(word);
        if (!said.contains(one))
            said.append(one);
    }
    return said.join(QStringLiteral(", "));
}

// The repair a value holding a word that is not a tag is named with: `tag
// clear`, which drops the row too where `row` says there is one.
QString clearHint(const QString &path, bool row)
{
    return QStringLiteral("`omafiles tag clear -- %1` drops the value%2")
        .arg(shellQuoted(path), row ? QStringLiteral(" and the row") : QString());
}

// The files' own copies of their tags, written once the rows they mirror are
// certainly on disk -- each path once, with the row the commit kept, since a
// batch can touch one path from several lines. A dry run makes the refusals
// this program decides for itself and writes nothing.
//
// Every value is read before it is written, and one holding a word that is not
// a tag is left as it is, unless `tag clear` dropped it: rewriting it would
// delete that word, and no other verb may.
//
// A value that already holds what would be written needs no write, and one
// whose valid tags already are the row's refuses nothing, except on a path a
// `reindex` walked, where the words beside them are the disagreement.
//
// Whether a copy not written is an error or a note follows the state the run
// ends in. It is part of what was promised on a path a `reindex` walked, and on
// one no row records before the run or after it -- there the file's value is
// the only record, so a drop it did not take did not happen. Those exit 1, and
// `--quiet` keeps what they say. Anywhere else the row is the record and the
// run exits 0, so it is a note.
void flushPending(const Globals &g, Session &s)
{
    s.mirrored.clear();
    // Each path at its last place in the queue, so where two names are one file
    // the copy it ends with is the row a line wrote last, as run line by line.
    QStringList order;
    QSet<QString> seen;
    for (auto queued = s.pending.crbegin(); queued != s.pending.crend(); ++queued) {
        if (!seen.contains(queued->path)) {
            seen.insert(queued->path);
            order.prepend(queued->path);
        }
    }
    // Per file, since two names of one share its copy: the value it began the
    // flush with, and the last one written to it, dry run too. A copy is
    // changed where the two differ, whatever it passed through.
    QHash<FileId, QByteArray> began;
    QHash<FileId, QByteArray> wrote;
    QHash<QString, FileId> written;
    for (const QString &path : std::as_const(order)) {
        const QStringList tags = s.store.get(path).tags;
        const bool unrecorded = tags.isEmpty() && s.store.loaded(path).path.isEmpty();
        QString refused;
        QByteArray value;
        const TagsValue held = readTagsValue(path, &value, &refused);
        // The row is the only record there, and nothing is said about it.
        if (held == TagsValue::Unsupported)
            continue;
        const std::optional<FileId> id = fileIdOf(path);
        if (id && wrote.contains(*id))
            value = wrote.value(*id);
        else if (id && !began.contains(*id))
            began.insert(*id, value);
        if (held != TagsValue::Unknown && value == tags.join(QLatin1Char(',')).toUtf8())
            continue;
        QStringList rejected;
        const QStringList valid = held == TagsValue::Held && !s.cleared.contains(path)
            ? salvagedTags(tagsOfValue(value), &rejected)
            : QStringList();
        if (!rejected.isEmpty() && valid == tags && !s.walked.contains(path))
            continue;
        if (!rejected.isEmpty()) {
            refused = QStringLiteral("%1 carries words that are not tags (%2), so its xattr "
                                     "was left as it is; %3")
                          .arg(spelled(path), saidWords(rejected),
                               clearHint(path, !s.store.get(path).path.isEmpty()));
        } else if (held != TagsValue::Unknown
                   && (g.dryRun || writeTagsXattr(path, tags, &refused))) {
            s.mirrored.insert(path);
            if (id) {
                wrote.insert(*id, tags.join(QLatin1Char(',')).toUtf8());
                written.insert(path, *id);
            }
            continue;
        }
        const QString said = QStringLiteral("%1: %2").arg(s.queuedBy.value(path, s.verb), refused);
        if (unrecorded || s.walked.contains(path)) {
            fail(said);
            s.incomplete = true;
        } else {
            warn(g, said);
        }
    }
    for (auto file = written.cbegin(); file != written.cend(); ++file) {
        if (wrote.value(file.value()) == began.value(file.value()))
            s.mirrored.remove(file.key());
    }
    s.pending.clear();
    s.queuedBy.clear();
    s.walked.clear();
    s.cleared.clear();
}

// Whether this run changed what is kept about `path`: its row, or the file's
// own copy the last flush wrote. A receipt lists only those, as `git add`
// reports nothing for a file already staged.
bool changedBy(const Session &s, const QString &path)
{
    return s.store.loaded(path) != s.store.get(path) || s.mirrored.contains(path);
}

// The commit, then the files' copies, then the receipt: under `--json` the
// rows this run changed, and under `--dry-run` the same set as paths.
// `receipt` is null for the verbs that print their own answer -- `mv` and `cp`
// say where the file went, and a row array beside that object would be a
// second document on the same stdout.
int settle(const Globals &g, Session &s, const QVector<Record> *receipt)
{
    QString error;
    if (!g.dryRun && !s.store.commit(&error)) {
        fail(error);
        return kStore;
    }
    flushPending(g, s);
    if (receipt && (g.dryRun || g.json)) {
        QVector<Record> changed;
        for (const Record &record : *receipt) {
            if (changedBy(s, record.path))
                changed.append(record);
        }
        printRows(g, changed);
    }
    return kOk;
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
    return settle(g, s, announce ? &written : nullptr);
}

// A verb's own status once its rows are settled: 1 where the run walked away
// from something it was asked to settle. Inside a batch that is read once, by
// the batch, after its commit.
int settledStatus(const Session &s)
{
    return s.incomplete && !s.deferred ? kIncomplete : kOk;
}

// The file's own copy of its tags, held back until the row that mirrors it has
// landed. The commit can still fail -- a read-only index, a full disk, a lock
// it never got -- and a file claiming tags no row records is exactly the
// disagreement `verify` exists to shout about.
//
// A dry run does not predict whether a filesystem will take a `user.*` write:
// every proxy is wrong in both directions -- `access(W_OK)` says no for root,
// and yes for a mount or an immutable flag that will refuse it anyway. So
// `reindex --dry-run` on a chmod 444 file prints the path and exits 0 where
// the real run exits 1.
//
// `clearing` is `tag clear`'s licence: see `flushPending`.
void mirrorTags(Session &s, const Record &record, bool clearing = false)
{
    if (const std::optional<FileId> id = fileIdOf(record.path))
        s.settled.insert(*id, record.path);
    s.pending.append(record);
    if (!s.line.isEmpty())
        s.queuedBy.insert(record.path, s.line);
    if (clearing)
        s.cleared.insert(record.path);
}

// The tags the flush writes onto the file at `path`, where a line of this run
// queued its copy by this name or another: what the file would hold by now had
// each line committed on its own.
std::optional<QStringList> settledCopy(const Session &s, const QString &path)
{
    const std::optional<FileId> id = fileIdOf(path);
    const auto found = id ? s.settled.constFind(*id) : s.settled.cend();
    if (found == s.settled.cend())
        return std::nullopt;
    return s.store.get(*found).tags;
}

// `row` holding `tags`, stamped now exactly where they are not the tags it
// held -- every tag written counts, and a write that changed none keeps the
// stamp, undated included. A path with no row is always a change. `loaded` is
// the row the index held when the run read it: tags a batch took away and put
// back are that row's, with its stamp. The stamp is the whole basis on which
// `recent` answers.
Record retagged(Record row, QStringList tags, const Record &loaded = {})
{
    tags = uniqueSorted(std::move(tags));
    if (!loaded.path.isEmpty() && tags == loaded.tags)
        row.tagged = loaded.tagged;
    else if (row.path.isEmpty() || tags != row.tags)
        row.tagged = QDateTime::currentSecsSinceEpoch();
    row.tags = tags;
    return row;
}

// Builds the row a write verb is about to store at `path`, over `before`, the
// row the index holds there, or none, and `loaded`, as `retagged` takes it.
//
// `state` is which way the check below went, for the caller that turns it into
// an exit code: every one of them resolved the path already, so a stat that
// fails here is the path going away, or the system going quiet about it,
// between then and now -- and those are still the two different answers.
Record stamped(const QString &path, QStringList tags, QString *error, PathState *state,
               const Record &before = {}, const Record &loaded = {})
{
    // Asked so the verb fails on a path the system will not talk about, rather
    // than writing a row about it.
    const PathState found = pathState(path, error);
    if (state)
        *state = found;
    if (found != PathState::Present)
        return {};
    Record record = retagged(before, std::move(tags), loaded);
    record.path = path;
    return record;
}

// Whether `source` reaches this process's fd 0, as `/dev/stdin`, `/dev/fd/0`
// and `/proc/self/fd/0` do, by a chain of links each resolved through its parent.
bool reachesStdin(const QString &source)
{
    const QString own = QStringLiteral("/proc/%1/fd/0").arg(::getpid());
    QString hop = source;
    // The kernel's own limit on a chain of links.
    for (int hops = 0; hops < 40; ++hops) {
        const QString named = canonicalLinkPath(hop, nullptr);
        if (named == own)
            return true;
        QString target;
        if (named.isEmpty()
            || !nameFromBytes(linkTarget(QFile::encodeName(named)), &target)
            || target.isEmpty())
            return false;
        hop = target.startsWith(QLatin1Char('/'))
            ? target
            : typedDirectory(named, nullptr) + QLatin1Char('/') + target;
    }
    return false;
}

// Whether `path` reaches a closed stdin, said as `-` over one is: `main` put
// /dev/null there, which is not what was named.
bool closedStdin(const QString &verb, const QString &path)
{
    if (!stdinClosed || !reachesStdin(path))
        return false;
    fail(QStringLiteral("%1: cannot read stdin: %2").arg(verb, systemReason(EBADF)));
    return true;
}

QStringList resolveAll(const QString &verb, const QStringList &paths, int *status,
                       QString (*resolve)(const QString &, QString *, PathState *))
{
    QStringList out;
    QSet<QString> seen;
    // Every operand it cannot resolve is named, not just the first, the way
    // coreutils `rm` names each one it cannot remove. The subset it could
    // resolve comes back with the status, so a caller with checks of its own
    // can name their offenders too. Two spellings of one path are one operand,
    // so no verb acts, or prints a receipt row, twice for it.
    for (const QString &path : paths) {
        if (closedStdin(verb, path)) {
            *status = qMax(*status, kIncomplete);
            continue;
        }
        QString error;
        PathState state = PathState::Missing;
        const QString canonical = resolve(path, &error, &state);
        if (canonical.isEmpty()) {
            fail(QStringLiteral("%1: %2").arg(verb, error));
            // A list holding one of each reports the missing path, because that
            // one is settled. See `pathStatus`.
            *status = qMax(*status, pathStatus(state));
            continue;
        }
        if (!seen.contains(canonical)) {
            seen.insert(canonical);
            out.append(canonical);
        }
    }
    return out;
}

// Through to whatever the path finally points at.
//
// This is what the tagging verbs want. On Linux a `user.*` xattr cannot be set
// on a symlink at all, so the file at the end of the link is the only thing
// `tag add` could mean, and a row filed under the link's name would be a second
// row for a file that already has one.
QStringList resolveExisting(const QString &verb, const QStringList &paths, int *status)
{
    return resolveAll(verb, paths, status, canonicalPath);
}

// `resolveExisting` for a verb that writes a file's tags, which refuses a file
// omafiles keeps: its next write renames a new file over it, copy and all.
QStringList resolveTagged(const QString &verb, const QStringList &paths, int *status)
{
    const QStringList resolved = resolveExisting(verb, paths, status);
    refuseKept(verb, resolved, status, false);
    return resolved;
}

// The path as named, link and all.
//
// For the verbs that act on the path itself rather than on the bytes at the end
// of it: POSIX `unlink()`, coreutils `rm` and `mv`, and `gio trash` all act on
// the link.
QStringList resolveNamed(const QString &verb, const QStringList &paths, int *status)
{
    return resolveAll(verb, paths, status, canonicalLinkPath);
}

// The tags a write to `path` starts from, decided here for every writer: the
// index's row plus the valid tags the file's own value carries -- `sweep`'s
// rule, so a write adds to what is known rather than writing over it -- and
// for a path this run already settled, the row alone, since its copy is
// rewritten from that row once the commit lands.
struct Starting {
    Record row;             // the index's row; path empty where it has none
    QStringList tags;       // what the write starts from
};

// False where the value cannot be read: taking that as no value would let the
// write destroy it. Said, unless `unread` takes the reason; `start` then holds
// the row alone.
bool startingRow(const Session &s, const QString &verb, const QString &path, Starting *start,
                 QString *unread = nullptr)
{
    start->row = s.store.get(path);
    start->tags = start->row.tags;
    if (const std::optional<QStringList> copy = settledCopy(s, path)) {
        start->tags = uniqueSorted(start->tags + *copy);
        return true;
    }
    QByteArray value;
    QString error;
    const TagsValue held = readTagsValue(path, &value, &error);
    if (held == TagsValue::Unknown) {
        if (unread)
            *unread = error;
        else
            fail(QStringLiteral("%1: %2; the tags it carries cannot be read, so nothing was "
                                "written").arg(verb, error));
        return false;
    }
    if (held == TagsValue::Held)
        start->tags = uniqueSorted(start->tags + salvagedTags(tagsOfValue(value), nullptr));
    return true;
}

// `tags` are folded already, and `paths` is not empty: `parseCommand` checks
// both, so a batch refuses a bad line before any line runs.
int cmdTagAdd(const Globals &g, Session &s, const QStringList &tags, const QStringList &paths)
{
    int status = kOk;
    const QStringList resolved = resolveTagged(QStringLiteral("tag add"), paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QString error;
    QVector<Record> written;
    for (const QString &path : resolved) {
        Starting start;
        if (!startingRow(s, QStringLiteral("tag add"), path, &start))
            return kIncomplete;
        // The file's own copy is written even where no tag changed: it is the
        // half that goes missing on its own -- a `cp` without
        // `--preserve=xattr`, a move onto a filesystem with no `user.*` -- so
        // skipping it would leave `verify` shouting about a disagreement this
        // verb is the repair for.
        PathState state = PathState::Missing;
        const Record record = stamped(path, start.tags + tags, &error, &state, start.row,
                                      s.store.loaded(path));
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        mirrorTags(s, record);
    }
    return finish(g, s, written);
}

int cmdTagRm(const Globals &g, Session &s, const QStringList &tags, const QStringList &paths)
{
    int status = kOk;
    const QStringList resolved = resolveTagged(QStringLiteral("tag rm"), paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QString error;
    QVector<Record> written;
    for (const QString &path : resolved) {
        Starting start;
        if (!startingRow(s, QStringLiteral("tag rm"), path, &start))
            return kIncomplete;
        QStringList next;
        for (const QString &tag : std::as_const(start.tags)) {
            if (!tags.contains(tag))
                next.append(tag);
        }
        PathState state = PathState::Missing;
        const Record record = stamped(path, next, &error, &state, start.row,
                                      s.store.loaded(path));
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        mirrorTags(s, record);
    }
    if ((status = finish(g, s, written)) != kOk)
        return status;
    return settledStatus(s);
}

int cmdTagLs(const Globals &g, Session &s, const QStringList &paths)
{
    int status = kOk;
    const QStringList resolved = resolveExisting(QStringLiteral("tag ls"), paths, &status);
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
// Where the suffix of `name` begins, at its dot, or -1 for none. Leading dots
// name the file rather than introducing a suffix, as Python's
// `os.path.splitext` reads them: `.cshrc` and `..d` have none, `.a.b` has `.b`.
qsizetype suffixDot(const QString &name)
{
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    qsizetype first = 0;
    while (first < name.size() && name.at(first) == QLatin1Char('.'))
        ++first;
    return dot > first ? dot : -1;
}

// `--ext hidden` does not match `.hidden`, a file whose extension is nothing,
// and Go settled it the same way (golang/go#24239). A folder is left out by
// `matchesShape`, which reads the file.
bool matchesExt(const QString &path, const QStringList &exts)
{
    if (exts.isEmpty())
        return true;
    const QString name = QFileInfo(path).fileName();
    const qsizetype dot = suffixDot(name);
    return dot >= 0 && exts.contains(name.mid(dot + 1).toLower());
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

// What `--type` and `--changed-within` ask of the file itself. `file` is
// anything that is not a folder, and a link counts as what it leads to, the
// way the window's Type column reads a row.
struct FileShape {
    QString kind;
    qint64 changedWithin = -1;
};

// `filesOnly` is `--ext`'s: a folder has no extension.
bool matchesShape(const QString &path, const FileShape &shape, bool filesOnly, QString *why)
{
    if (!filesOnly && shape.kind.isEmpty() && shape.changedWithin < 0)
        return true;
    // Through a link, and only a link that leads nowhere is itself: a file, GNU
    // `find -L`'s rule. A name gone since it was listed has no kind to match,
    // and one the system will not describe -- a link loop, a target shut to us
    // -- is said in `why`, because the list is then short.
    struct stat st {};
    PathState found = statState(path, why, true, &st);
    if (found == PathState::Missing)
        found = statState(path, why, false, &st);
    if (found != PathState::Unknown)
        why->clear();
    if (found != PathState::Present)
        return false;
    const bool folder = S_ISDIR(st.st_mode);
    if ((filesOnly && folder)
        || (!shape.kind.isEmpty()
            && shape.kind != (folder ? QLatin1String("dir") : QLatin1String("file"))))
        return false;
    return shape.changedWithin < 0
        || st.st_mtime >= QDateTime::currentSecsSinceEpoch() - shape.changedWithin;
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

// The `[dir]` a verb was handed, resolved and judged like any operand, so one
// under a root that is away is refused. Empty where none was named.
QString optionalDir(const QString &verb, const QStringList &positionals, int *status)
{
    if (positionals.isEmpty())
        return {};
    if (closedStdin(verb, positionals.first())) {
        *status = kIncomplete;
        return {};
    }
    QString error;
    PathState state = PathState::Missing;
    const QString dir = canonicalPath(positionals.first(), &error, &state);
    if (dir.isEmpty()) {
        fail(QStringLiteral("%1: %2").arg(verb, error));
        // The same table an operand takes: a `[dir]` nobody may look at is not
        // a `[dir]` that is not there. See `pathStatus`.
        *status = pathStatus(state);
        return {};
    }
    struct stat st {};
    if (statState(dir, nullptr, true, &st) == PathState::Present && !S_ISDIR(st.st_mode)) {
        fail(QStringLiteral("%1: not a directory: %2").arg(verb, spelled(positionals.first())));
        *status = kUsage;
        return {};
    }
    return dir;
}

// `optionalDir`, with the current directory where none was named: `verify`,
// `reindex`, `prune` and a walking `ls` answer about where they stand.
QString dirOrHere(const QString &verb, const QStringList &positionals, int *status)
{
    return optionalDir(verb,
                       positionals.isEmpty() ? QStringList{QStringLiteral(".")} : positionals,
                       status);
}

// `optionalDir` only asks whether the name is a directory. stat still
// answers for a chmod 000, so a verb asks what it needs of the directory: a
// walk lists it, which is read, on the rule `walkInto` uses, and an answer out
// of the index stats the rows under it, which is search alone. What the system
// said, or empty where it may.
QString cannotLookInto(const QString &dir, bool walking)
{
    QString why;
    PathState state = PathState::Present;
    if (dir.isEmpty())
        return {};
    if (!walking) {
        const QString inside = QDir(dir).filePath(QStringLiteral("."));
        state = statState(inside, &why);
        return state == PathState::Present || state == PathState::Missing
            ? QString()
            : reasonOnly(why, inside);
    }
    if (readDirectory(dir, nullptr, nullptr, &why, &state) || state == PathState::Missing)
        return {};
    return reasonOnly(why, dir);
}

// Whether the name at the end of `path` fits a shell pattern -- on the name
// alone, since the directories above it are what `[dir]` is for.
bool matchesName(const QString &path, const QString &pattern)
{
    return pattern.isEmpty() || nameFits(pattern, QFileInfo(path).fileName());
}

int cmdLs(const Globals &g, Session &s, const QStringList &ands, const QStringList &anys,
           bool untagged, bool cached, const QString &namePattern, const QStringList &exts,
           const FileShape &shape, qint64 sinceSeconds, const QStringList &positionals)
{
    // `--name` alone is a question about the disk, the way `--untagged` is:
    // with no tag in the question there is no reason to answer out of the
    // index, which only knows the files that carry one. Beside a tag, or
    // `--since`, it narrows the rows those already chose.
    const bool askingDisk = untagged
        || (!namePattern.isEmpty() && ands.isEmpty() && anys.isEmpty() && sinceSeconds < 0);
    int status = kOk;
    const bool walks = askingDisk && !cached;
    const QString dir = walks ? dirOrHere(QStringLiteral("ls"), positionals, &status)
                              : optionalDir(QStringLiteral("ls"), positionals, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QVector<Record> hits;
    int undated = 0;
    // What the answer could not cover, one sentence each: every one makes the
    // list short, and is said on stderr and exits 1. `refused` is the same, and
    // an error, so `--quiet` keeps it.
    QStringList gaps;
    QStringList refused;
    // A name out of the list or the walk, with the row it has, so a `--name`
    // answer carries the tags the file wears. On a walk a link now at a row's
    // path is not its file, so it is untagged; out of the list nothing is
    // looked at, and the row stands as the index holds it.
    const auto takeName = [&](const QString &path) {
        Record record = s.store.get(path);
        if (walks && !record.path.isEmpty() && rowState(path, nullptr) == PathState::Missing)
            record = {};
        if (untagged && !record.tags.isEmpty())
            return;
        if (!matchesName(path, namePattern) || !matchesExt(path, exts))
            return;
        if (QString why; !matchesShape(path, shape, !exts.isEmpty(), &why)) {
            if (!why.isEmpty())
                gaps.append(QStringLiteral("%1; it was not listed").arg(why));
            return;
        }
        Record row = record;
        if (row.path.isEmpty())
            row.path = path;
        hits.append(row);
    };
    if (askingDisk && cached) {
        // The whole point is that nothing here touches the disk: no walk, and
        // no `stat` on the way out either. What comes back is what the tree
        // held when the list was built, which is why the age is said out loud
        // below and why plain `--untagged` stays exact.
        // Every list that can hold a path under `[dir]`: the root holding it
        // and every root declared inside it. Each path counts under its own
        // root's list alone, so a nested root is neither listed twice nor
        // listed while it is away. A `[dir]` no root holds has no list for
        // the part of it outside them.
        const Root *holding = dir.isEmpty() ? nullptr : rootFor(dir);
        QVector<Root> scope;
        for (const Root &root : roots()) {
            if (dir.isEmpty()
                || (holding && (&root == holding || isUnderDirectory(root.path, dir))))
                scope.append(root);
        }
        if (roots().isEmpty()) {
            fail(QStringLiteral("ls: --cached answers out of a root's list of names, "
                                "and no root is declared; `omafiles roots add` declares "
                                "the trees this machine keeps tags on"));
            return kUsage;
        }
        if (scope.isEmpty()) {
            fail(QStringLiteral("ls: %1 is under no declared root, so there is no list "
                                "of names covering it; the same question without "
                                "--cached walks it")
                     .arg(spelled(dir)));
            return kUsage;
        }

        qint64 oldest = -1;
        for (const Root &root : std::as_const(scope)) {
            // A disk in a drawer has a list and the paths in it are not
            // reachable. Saying so is the same answer every other verb gives.
            QString error;
            const PathState storage = rootState(root, &error);
            if (storage == PathState::Unreachable) {
                gaps.append(QStringLiteral("root %1 is declared and not here, so "
                                           "nothing under it was listed")
                                .arg(root.id));
                continue;
            }
            if (storage != PathState::Present) {
                gaps.append(QStringLiteral("root %1 cannot be checked: %2, so nothing "
                                           "under it was listed")
                                .arg(root.id, error));
                continue;
            }
            QStringList names;
            QVector<Gap> lacking;
            qint64 age = -1;
            if (!readNames(root, &names, &age, &error, &lacking, &s.store)) {
                refused.append(error);
                continue;
            }
            if (age < 0) {
                gaps.append(QStringLiteral("root %1 has no list of names yet, so "
                                           "nothing under it was listed; "
                                           "`omafiles sweep` builds one")
                                .arg(root.id));
                continue;
            }
            oldest = qMax(oldest, age);
            // The walk that built the list was short, and so is an answer out
            // of it where the gap is `[dir]` or inside it, as the walk would have
            // been. A gap with no path is about the whole list.
            for (const Gap &gap : std::as_const(lacking)) {
                if (gap.path.isEmpty() || dir.isEmpty() || isUnderDirectory(gap.path, dir))
                    gaps.append(QStringLiteral("the list of names is short: %1").arg(gap.said));
            }
            for (const QString &path : std::as_const(names)) {
                // The walk does not yield the directory it was pointed at, so
                // neither does this: `ls --untagged ~/docs` is about what is in
                // ~/docs.
                if (!dir.isEmpty() && (path == dir || !isUnderDirectory(path, dir)))
                    continue;
                if (!listedUnder(path, root))
                    continue;
                takeName(path);
            }
        }
        // Said whatever the age turns out to be, because the contract is that
        // this answer is as old as the last sweep and not that it is fresh. A
        // note beside a run that succeeded, so `--quiet` takes it away.
        if (oldest >= 0) {
            warn(g, QStringLiteral("ls: from a list of names built %1 ago; "
                                   "`omafiles sweep` rebuilds it, and the same question "
                                   "without --cached walks the disk and is exact")
                        .arg(spelledDuration(oldest)));
        }
    } else if (askingDisk) {
        // `[dir]` is optional, and `verify`, `reindex` and `prune` all read an
        // absent one as the current directory.
        const Walk walk = walkTree(dir);
        for (const Gap &gap : walkGaps(walk, QStringLiteral("listed")))
            gaps.append(gap.said);
        // Every entry by the name it has, the way `find -name` lists: a link is
        // itself, and carries no row of its own because a tag on it is on the
        // file it leads to.
        for (const QString &path : walk.paths)
            takeName(path);
    } else {
        if (const QString why = cannotLookInto(dir, false); !why.isEmpty())
            gaps.append(shutGap(dir, QStringLiteral("listed"), why));
        for (const Record &record : s.store.records()) {
            if (!dir.isEmpty() && !isUnderDirectory(record.path, dir))
                continue;
            if (!matchesTags(record, ands, anys))
                continue;
            if (!matchesName(record.path, namePattern) || !matchesExt(record.path, exts))
                continue;
            // Asked after the other filters for the same reason the count
            // below is: a row this query was never going to return is not a
            // row to say anything about, and an unplugged drive holding rows
            // in another tree would otherwise bury the answer in notes.
            QString error;
            const PathState state = rowState(record.path, &error);
            // "I may not look" is not "it is gone". The row is still skipped
            // -- nothing here can list what the system will not describe -- but
            // it is skipped out loud.
            if (state == PathState::Unknown || state == PathState::Unreachable) {
                gaps.append(QStringLiteral("%1; it was not listed").arg(error));
                continue;
            }
            if (state == PathState::Missing)
                continue;
            if (!matchesShape(record.path, shape, !exts.isEmpty(), &error)) {
                if (!error.isEmpty())
                    gaps.append(QStringLiteral("%1; it was not listed").arg(error));
                continue;
            }
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

    printRows(g, hits);
    for (const QString &gap : std::as_const(gaps))
        warn(g, QStringLiteral("ls: %1").arg(gap));
    for (const QString &error : std::as_const(refused))
        fail(QStringLiteral("ls: %1").arg(error));
    if (undated > 0) {
        warn(g, QStringLiteral("ls: %1 matching row(s) carry no tagged stamp, so "
                               "--since cannot judge them; they are stamped the next "
                               "time their tags change")
                    .arg(undated));
    }
    // A short list is not the answer that was asked for, and a note on stderr
    // is not enough on its own, because `--quiet` is allowed to take it away.
    // The undated count is not one of these: those rows are there and the index
    // simply cannot date them, which is a narrower answer rather than a shorter
    // one. See `kIncomplete`.
    return gaps.isEmpty() && refused.isEmpty() ? kOk : kIncomplete;
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
    // reason, and quotes it the way `rm -i` does, so a name holding a newline
    // stays one line.
    QString named;
    for (int i = 0; i < count && i < kNamedInPrompt; ++i)
        named += QStringLiteral("\n  %1").arg(shellQuoted(paths.at(i)));
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
    const QString dir = optionalDir(QStringLiteral("tags"), positionals, &status);
    if (status != kOk)
        return status;
    if ((status = openSession(&s)) != kOk)
        return status;

    const QString scopeShut = cannotLookInto(dir, false);
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
        const PathState state = rowState(record.path, &error);
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
    // The tree's gap before the rows', the order `ls` says them in.
    if (!scopeShut.isEmpty()) {
        warn(g, QStringLiteral("tags: %1")
                    .arg(shutGap(dir, QStringLiteral("counted"), scopeShut)));
    }
    for (const QString &error : skipped) {
        warn(g, QStringLiteral("tags: %1; the row was not counted").arg(error));
    }
    // A vocabulary short of the tags only those rows carry is not the
    // vocabulary. See `kIncomplete`.
    return !scopeShut.isEmpty() || !skipped.isEmpty() ? kIncomplete : kOk;
}

// The `[dir]` of a `prune` that is gone from the disk and still holds rows at
// or under it -- the tree a `mv` by another program left -- named as `realpath
// -m` names it, the way `git rm -r --cached` matches a gone directory against
// the index. Empty where the operand is here, or gone with no rows, which the
// ordinary `[dir]` rule then answers.
QString goneScope(Session &s, const QStringList &positionals, int *status)
{
    if (positionals.isEmpty())
        return {};
    QString error;
    PathState state = PathState::Present;
    if (!canonicalPath(positionals.first(), &error, &state).isEmpty()
        || state != PathState::Missing || !danglingChain(positionals.first()).isEmpty())
        return {};
    const QString named = storedSpelling(physicalPath(positionals.first()));
    if (named.isEmpty() || QDir::cleanPath(named) != named)
        return {};
    if ((*status = openSession(&s)) != kOk)
        return {};
    for (const Record &record : s.store.records()) {
        if (isUnderDirectory(record.path, named))
            return named;
    }
    return {};
}

// Drops rows whose file is gone, inside a tree -- or across the whole index,
// but only when asked in so many words. An absent directory means the current
// one, as it does for `verify` and `reindex`; `--all` is the whole index,
// spelled out, so the one destructive verb here is not the one with the widest
// reach by default.
int cmdPrune(const Globals &g, Session &s, bool all, const QStringList &positionals)
{
    int status = kOk;
    QString dir = all ? QString() : goneScope(s, positionals, &status);
    if (status != kOk)
        return status;
    if (!all && dir.isEmpty())
        dir = dirOrHere(QStringLiteral("prune"), positionals, &status);
    if (status != kOk)
        return status;
    if ((status = openSession(&s)) != kOk)
        return status;

    if (const QString why = all ? QString() : cannotLookInto(dir, false); !why.isEmpty()) {
        warn(g, QStringLiteral("prune: %1").arg(shutGap(dir, QStringLiteral("dropped"), why)));
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
        const PathState state = rowState(record.path, &error);
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
                             : QStringLiteral("prune (under %1)").arg(spelled(dir));
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

    // Inside a batch the removals wait for the one commit at the end, and its
    // receipt names them.
    if (s.deferred) {
        for (const QString &path : std::as_const(dropped)) {
            Record gone;
            gone.path = path;
            gone.tagged = QDateTime::currentSecsSinceEpoch();
            s.written.append(gone);
        }
        return kOk;
    }
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
    // Every path resolved, then every one planned, before any of them is
    // launched: a launched program cannot be un-launched. Every path that is
    // not there is named first; a missing opener is the same answer for every
    // operand, so it is given once, after them.
    int status = kOk;
    const QStringList resolved = resolveExisting(QStringLiteral("open"), paths, &status);
    if (status != kOk)
        return status;
    QVector<OpenPlan> plans;
    for (const QString &path : resolved) {
        OpenPlan plan;
        QString error;
        PathState state = PathState::Present;
        if (!planOpen(path, &plan, &error, &state)) {
            fail(error);
            return state == PathState::Present ? kUsage : pathStatus(state);
        }
        // An empty field is a silent "don't know".
        if (g.json && plan.mime.isEmpty())
            warn(g, QStringLiteral("open: xdg-mime could not say what %1 is, so its mime "
                                   "is empty").arg(spelled(path)));
        plans.append(plan);
    }

    QJsonArray opened;
    // Past the gate, every one of them -- and a launch the desktop refused does
    // not cost the rest theirs, the way `rm` reports the operand it could not
    // remove and still removes the others.
    bool refused = false;
    for (int i = 0; i < resolved.size(); ++i) {
        if (!g.dryRun) {
            QString error;
            if (!openLocalPath(resolved.at(i), &error)) {
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


int cmdRecent(const Globals &g, Session &s, bool desktop, const QStringList &positionals)
{
    int limit = 20;
    if (!positionals.isEmpty()) {
        // coreutils `head -n`: leading blanks and a `+`, nothing after the
        // digits, and a count wider than it can hold read as the widest.
        static const QRegularExpression digits(
            QRegularExpression::anchoredPattern(QStringLiteral("\\s*\\+?[0-9]+")));
        bool ok = false;
        const bool digitsOnly = digits.match(positionals.first()).hasMatch();
        const qint64 count = positionals.first().toLongLong(&ok);
        const bool wide = digitsOnly && !ok;
        if (!digitsOnly || (!wide && count <= 0)) {
            fail(QStringLiteral("recent: %1 is not a count").arg(spelled(positionals.first())));
            return kUsage;
        }
        constexpr qint64 widest = std::numeric_limits<int>::max();
        limit = static_cast<int>(wide ? widest : qMin(count, widest));
    }

    int status = openSession(&s);
    if (status != kOk)
        return status;

    QVector<Record> rows;
    QStringList skipped;
    int unknown = 0;
    // The desktop's list, in the desktop's order, with the row each file has
    // -- the way `ls --name` carries tags across an answer the index did not
    // choose. The reader already dropped what is not there.
    if (desktop) {
        QVector<RecentEntry> opened;
        QString error;
        if (!recentlyUsed(&opened, &skipped, &error, limit)) {
            fail(QStringLiteral("recent: %1").arg(error));
            return kUsage;
        }
        for (const RecentEntry &entry : std::as_const(opened)) {
            Record record = s.store.get(entry.path);
            if (record.path.isEmpty())
                record.path = entry.path;
            rows.append(record);
        }
    } else {
        QVector<Record> dated;
        for (const Record &record : s.store.records()) {
            if (record.tagged > 0)
                dated.append(record);
            else if (rowState(record.path, nullptr) != PathState::Missing)
                ++unknown;
        }
        std::sort(dated.begin(), dated.end(), [](const Record &a, const Record &b) {
            if (a.tagged != b.tagged)
                return a.tagged > b.tagged;
            return a.path < b.path;
        });
        // Ordered by the stamps the index holds and looked at after, down to
        // the limit, so a row below it costs the answer nothing. A path the
        // system will not talk about is not a path that is gone: a list of the
        // newest things is read by what is at the top of it, which is where a
        // silently dropped row hides best.
        for (const Record &record : std::as_const(dated)) {
            if (rows.size() >= limit)
                break;
            QString error;
            const PathState state = rowState(record.path, &error);
            if (state == PathState::Missing)
                continue;
            if (state != PathState::Present) {
                skipped.append(error);
                continue;
            }
            rows.append(record);
        }
    }

    if (unknown > 0) {
        warn(g, QStringLiteral("recent: %1 row(s) carry no tagged stamp and cannot be "
                               "ordered; they are stamped the next time their tags change")
                    .arg(unknown));
    }
    for (const QString &error : skipped) {
        warn(g, QStringLiteral("recent: %1; it was not listed").arg(error));
    }

    printRows(g, rows);
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
// The index's tags, for a position a tag goes in; none where the index will
// not read. A roots file that will not parse costs only the rows placed under a
// root.
std::optional<QStringList> tagCandidates()
{
    Store store(defaultIndexPath());
    QStringList tags;
    if (store.load(nullptr)) {
        for (const Record &record : store.records())
            tags.append(record.tags);
        return uniqueSorted(tags);
    }
    QString why;
    QVector<KeptLine> lines;
    if (loadRoots(&why) || !readKeptLines(defaultIndexPath(), QString(), &lines, &why))
        return std::nullopt;
    for (const KeptLine &line : std::as_const(lines)) {
        const QJsonValue root = line.object.value(QStringLiteral("root"));
        if (root.isString() && !rootById(root.toString()))
            continue;
        const Record record = keptRow(line.object, &why);
        if (record.path.isEmpty())
            return std::nullopt;
        tags.append(record.tags);
    }
    return uniqueSorted(tags);
}

// `words` is everything after the program name, with the partly typed word
// last. No answer means "no opinion", which is where a path goes, and the stub
// falls back to the shell's own filename completion; an empty answer is a
// position that takes something with nothing to offer, or takes nothing.
//
// Every rule is the parse's own -- `takesFor`, `kFlags`, `repeats`,
// `kConflicts` and `kNeeds` -- so a word is offered only where it will be taken,
// as cobra and clap_complete derive their completion from the command's
// definition. A word the verb would refuse makes the line an error already, and
// there completion has no opinion.
std::optional<QStringList> completionFor(const QStringList &words)
{
    static const QStringList verbs = {
        QStringLiteral("tag"),    QStringLiteral("ls"),      QStringLiteral("tags"),
        QStringLiteral("has"),    QStringLiteral("json"),    QStringLiteral("recent"),
        QStringLiteral("open"),   QStringLiteral("mv"),      QStringLiteral("cp"),
        QStringLiteral("trash"),  QStringLiteral("rm"),      QStringLiteral("chmod"),
        QStringLiteral("prune"),
        QStringLiteral("reindex"), QStringLiteral("sweep"),   QStringLiteral("verify"),
        QStringLiteral("export"),
        QStringLiteral("roots"),  QStringLiteral("view"),
        QStringLiteral("import"), QStringLiteral("batch"),   QStringLiteral("completions")};

    QString current = words.isEmpty() ? QString() : words.last();
    // A value typed attached, `--tag=wo`: the flag is what comes before, and an
    // attached candidate is offered with its flag in front.
    QStringList before = attachedSplit(words.mid(0, qMax(0, words.size() - 1)));
    QString attached;
    const int equals = attachedAt(current);
    if (equals > 0) {
        attached = current.left(equals + 1);
        before.append(current.left(equals));
        current = current.mid(equals + 1);
    }
    const auto values = [&attached](QStringList offered) {
        for (QString &value : offered)
            value.prepend(attached);
        return offered;
    };

    // The globals already on the line, in either spelling, read as the run
    // reads them: saying one twice asks nothing more, and a flag's value is
    // not one.
    QStringList said;
    before = withoutGlobals(before, &said);
    QStringList unsaid;
    for (const auto &[brief, spelled] : kGlobals) {
        if (std::none_of(said.cbegin(), said.cend(), [&](const QString &word) {
                return globalFlag(word) == QLatin1String(spelled);
            }))
            unsaid.append(QString::fromLatin1(spelled));
    }
    const bool json = !unsaid.contains(QLatin1String("--json"));

    // The verb, and the subcommand where the verb has them: the word right
    // after it, as the parse reads it. `takesFor` reads none as `ls`.
    QString verb;
    QString sub;
    int next = 0;
    if (!before.isEmpty()) {
        const QString &word = before.first();
        // The program refuses `--` before a verb, so nothing goes after it.
        if (word == QLatin1String("--"))
            return QStringList();
        // Past the globals the parse takes the next word for the verb, and
        // refuses a flag or a word that is none.
        if (!verbs.contains(word))
            return std::nullopt;
        verb = word;
        next = 1;
    }
    if (verb.isEmpty()) {
        // A value attached to a flag no global takes has nothing to offer.
        if (!attached.isEmpty())
            return std::nullopt;
        QStringList offered = unsaid;
        for (const QString &candidate : verbs) {
            if (!json || !takesFor(candidate).noJson)
                offered.append(candidate);
        }
        return offered;
    }
    const bool subNext = !subcommandsOf(verb).isEmpty() && next == before.size();
    if (!subcommandsOf(verb).isEmpty() && next < before.size()
        && !before.at(next).startsWith(QLatin1Char('-')))
        sub = before.at(next++);
    const Takes takes = takesFor(verb, sub);
    const QString named = sub.isEmpty() ? verb : verb + QLatin1Char(' ') + sub;
    if (json && takes.noJson)
        return QStringList();

    // What the rest of the line has said, read the way `parseFlags` reads it.
    QSet<QString> given;
    int operands = 0;
    bool dashes = false;
    const Flag *valueOf = nullptr;
    for (int i = next; i < before.size(); ++i) {
        const QString &word = before.at(i);
        if (dashes || (!word.startsWith(QLatin1Char('-')) || word == QLatin1String("-"))) {
            ++operands;
            continue;
        }
        if (word == QLatin1String("--")) {
            dashes = true;
            continue;
        }
        // A flag the verb does not take, one said twice that is said once, or
        // one beside a flag it may not be: the parse refuses the line already.
        const Flag *flag = flagNamed(word);
        if (!flag || !(takes.*flag->gate)
            || (given.contains(flag->name) && !repeats(*flag, takes))
            || pairedConflict(named, flag->name, given))
            return std::nullopt;
        given.insert(flag->name);
        if (flag->value == Value::None)
            continue;
        if (i + 1 == before.size())
            valueOf = flag;
        ++i;
    }

    if (valueOf) {
        switch (valueOf->value) {
        case Value::Tag: {
            const std::optional<QStringList> tags = tagCandidates();
            return tags ? std::optional(values(*tags)) : std::nullopt;
        }
        case Value::Kind:
            return values({QStringLiteral("dir"), QStringLiteral("file")});
        case Value::Path:
            return std::nullopt;
        default:
            break;
        }
        // The names a removal can take, the way cobra and git complete their
        // own objects; unknown where the file will not read.
        if (named == QLatin1String("view rm")) {
            QVector<View> views;
            if (!loadViews(&views, nullptr))
                return std::nullopt;
            QStringList names;
            for (const View &view : std::as_const(views))
                names.append(view.name);
            return values(names);
        }
        if (named == QLatin1String("roots rm")) {
            if (!loadRoots(nullptr))
                return std::nullopt;
            QStringList ids;
            for (const Root &root : roots())
                ids.append(root.id);
            return values(ids);
        }
        return QStringList();
    }

    // Whether `flag` would be refused beside what is on the line.
    const auto clashes = [&](const Flag &flag) {
        return (given.contains(flag.name) && !repeats(flag, takes))
            || !conflictOf(named, flag.name, given, operands > 0).isEmpty();
    };

    // A word that begins with `-` is a request for flags, even where a
    // subcommand or a shell name would otherwise be offered: cargo's script
    // tests `[[ "$cur" == -* ]]` before the subcommand list, and cobra hands
    // back flag names as soon as `toComplete[0] == '-'`.
    if (current.startsWith(QLatin1Char('-')) && !dashes) {
        QStringList flags;
        for (const Flag &flag : kFlags) {
            if (takes.*flag.gate && !clashes(flag))
                flags.append(flag.name);
        }
        for (const QString &global : std::as_const(unsaid)) {
            if (global != QLatin1String("--json") || !takes.noJson)
                flags.append(global);
        }
        return uniqueSorted(flags);
    }
    if (subNext)
        return subcommandsOf(verb);

    // An operand: none past the last the verb takes, or beside a flag that
    // takes the place of one.
    const bool taken = std::any_of(std::begin(kConflicts), std::end(kConflicts),
                                   [&](const Conflict &conflict) {
                                       return conflict.verb == named && conflict.other.isEmpty()
                                           && given.contains(conflict.flag);
                                   });
    const Takes counted = bySet(takes, given);
    if (taken || (counted.most >= 0 && operands >= counted.most))
        return QStringList();
    switch (operands == 0 ? counted.first : counted.rest) {
    case Operand::Tag:
        return tagCandidates();
    case Operand::Shell:
        return QStringList{QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("fish")};
    case Operand::Word:
        return QStringList();
    case Operand::Path:
        break;
    }
    return std::nullopt;                         // a path: the shell knows better
}

int cmdComplete(const QStringList &words)
{
    const std::optional<QStringList> candidates = completionFor(words);
    if (!candidates)
        return 1;                                // no opinion; let the shell try files
    for (const QString &candidate : *candidates)
        out() << candidate << '\n';
    return kOk;
}

// The stub is ten lines because the thinking is in the binary. What ships in
// /usr/share is a shim that outlives every change to the command set, and a
// shell that has omafiles has everything the completion needs -- no second tool
// to install, and no completions that silently stop working when it is missing.
int cmdCompletions(const QStringList &positionals)
{
    const QString shell = positionals.first();
    if (shell == QLatin1String("bash")) {
        out() << R"SHELL(# omafiles completion for bash
# The word as the shell will read it: quotes and backslashes undone, a quote
# still open read to the end of the word, as git-completion's __git_dequote.
_omafiles_dequote() {
    local i c quote=
    REPLY=
    for ((i = 0; i < ${#1}; i++)); do
        c=${1:i:1}
        if [[ $c == "$quote" ]]; then
            quote=
        elif [[ $quote == "'" ]]; then
            REPLY+=$c
        elif [[ $c == \\ && ( -z $quote || ${1:i+1:1} == [\$\`\"\\] ) ]]; then
            REPLY+=${1:i+1:1}
            ((++i))
        elif [[ -z $quote && $c == [\'\"] ]]; then
            quote=$c
        else
            REPLY+=$c
        fi
    done
}
_omafiles() {
    local i word piece cur typed offered candidate
    local -a words=() candidates=()
    COMPREPLY=()
    # `=` and `:` are in COMP_WORDBREAKS, so `key=v` arrives as three words;
    # joined again as git-completion's __git_reassemble_comp_words_by_ref does.
    for ((i = 1; i <= COMP_CWORD; i++)); do
        word=${COMP_WORDS[i]}
        if ((i > 1)) && [[ $word =~ ^[=:]+$ || ${COMP_WORDS[i-1]} =~ ^[=:]+$ ]]; then
            words[-1]+=$word
        else
            words+=("$word")
        fi
    done
    # Readline replaces only its own piece of the word: past the last `=` or
    # `:`, and past a quote still open.
    piece=${COMP_WORDS[COMP_CWORD]}
    [[ $piece =~ ^[=:]+$ ]] && piece=
    _omafiles_dequote "${words[-1]}"
    cur=$REPLY
    _omafiles_dequote "${words[-1]%"$piece"}"
    typed=$REPLY
    if offered=$(omafiles __complete -- "${words[@]:0:${#words[@]}-1}" "$cur" 2>/dev/null); then
        # Matched here and quoted with %q, as bash-completion 2.x does: compgen -W
        # would expand each word again, and a view named `my view` is one word.
        # Inside an open quote readline quotes nothing and closes it itself.
        # Case aside, since a tag is folded before anything else sees it.
        [[ $offered ]] && mapfile -t candidates <<<"$offered"
        for candidate in "${candidates[@]}"; do
            [[ ${candidate,,} == "${cur,,}"* ]] || continue
            candidate=${candidate#"$typed"}
            [[ $piece == [\'\"]* ]] || printf -v candidate %q "$candidate"
            COMPREPLY+=("$candidate")
        done
    else
        # Filename treatment for paths alone, as bash-completion's _filedir
        # turns it on: set on `complete`, a tag named like a folder gets a `/`.
        # mapfile, since an unquoted $(compgen -f) expands `[ab].txt` as a glob.
        # compgen -f undoes the quoting itself. A path attached to a flag,
        # `--from=a`, is the part after the `=`, as bash-completion's `-s` takes it.
        compopt -o filenames 2>/dev/null
        word=${words[-1]}
        [[ $word == --*=* ]] && word=${word#*=}
        mapfile -t candidates < <(compgen -f -- "$word")
        for candidate in "${candidates[@]}"; do
            # Readline marks a directory by testing its own piece, which past a
            # `:` or `=` is not the path, so the `/` is added here, as scp's does.
            [[ $typed && -d $candidate ]] && candidate+=/
            COMPREPLY+=("${candidate#"$typed"}")
        done
        [[ ${#COMPREPLY[@]} == 1 && ${COMPREPLY[0]} == */ ]] && compopt -o nospace 2>/dev/null
    fi
}
complete -F _omafiles omafiles
)SHELL";
        return kOk;
    }
    if (shell == QLatin1String("zsh")) {
        out() << R"SHELL(#compdef omafiles
_omafiles() {
    local offered
    # "${(@)...}" keeps the empty last word. Unquoted, zsh drops it, and the
    # binary then completes the previous word instead of the one being typed --
    # so `--tag <TAB>` offered flags where it should offer your tags. Up to
    # CURRENT, the word under the cursor, and not the rest of the line.
    if offered=$(omafiles __complete -- "${(@)words[2,CURRENT]}" 2>/dev/null); then
        compadd -- ${(f)offered}
    else
        [[ $PREFIX == --*=* ]] && compset -P 1 '*='
        _files
    fi
}
# Loaded from $fpath, this file is the body of `_omafiles` and the first Tab
# runs it; sourced, it registers. Cobra's guard.
if [ "$funcstack[1]" = "_omafiles" ]; then
    _omafiles "$@"
else
    compdef _omafiles omafiles
fi
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
        set -l flag (string match -r -- '^--[^=]*=' $cur)
        if set -q flag[1]
            printf '%s\n' $flag(__fish_complete_path (string replace -- $flag '' $cur))
        else
            __fish_complete_path $cur
        end
    end
end
complete -c omafiles -f -a '(__omafiles_complete)'
)SHELL";
        return kOk;
    }
    fail(QStringLiteral("completions: unknown shell %1 (bash, zsh, fish)").arg(spelled(shell)));
    return kUsage;
}

// ---------------------------------------------------------------- destructive

// Commits here rather than through `finish`.
//
// It has already printed the rows it dropped, and `settle` prints the rows
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
    int status = kOk;
    const QStringList resolved = resolveTagged(QStringLiteral("tag clear"), paths, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QString error;
    QVector<Record> written;
    for (const QString &path : resolved) {
        Starting start;
        if (!startingRow(s, QStringLiteral("tag clear"), path, &start))
            return kIncomplete;
        PathState state = PathState::Missing;
        const Record record = stamped(path, {}, &error, &state, start.row,
                                      s.store.loaded(path));
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        mirrorTags(s, record, true);
    }
    if ((status = finish(g, s, written)) != kOk)
        return status;
    return settledStatus(s);
}

// `from` and `paths` are there: `parseCommand` checks.
int cmdTagCp(const Globals &g, Session &s, const QString &from, const QStringList &paths)
{
    int status = kOk;
    const QStringList source = resolveTagged(QStringLiteral("tag cp"), {from}, &status);
    if (status != kOk)
        return status;
    const QStringList resolved = resolveTagged(QStringLiteral("tag cp"), paths, &status);
    if (status != kOk)
        return status;
    // coreutils `cp a a`: "are the same file", and nothing is copied.
    const std::optional<FileId> sourceFile = fileIdOf(source.first());
    for (const QString &path : resolved) {
        if (sourceFile && fileIdOf(path) == sourceFile) {
            fail(QStringLiteral("tag cp: %1 is the file the tags come from").arg(spelled(path)));
            return kUsage;
        }
    }

    if ((status = openSession(&s)) != kOk)
        return status;

    Starting origin;
    if (!startingRow(s, QStringLiteral("tag cp"), source.first(), &origin))
        return kIncomplete;
    // Nothing to copy is a mistake, not an instruction: copying from a file
    // with no tags cannot mean "clear the destination", so it is refused.
    if (origin.tags.isEmpty()) {
        fail(QStringLiteral("tag cp: %1 has no tags to copy").arg(spelled(source.first())));
        return kUsage;
    }

    QString error;
    QVector<Record> written;
    for (const QString &path : resolved) {
        // Added to what is already there, not written over it: `cp` names the
        // direction the tags travel, not a promise to make the two files
        // identical.
        Starting start;
        if (!startingRow(s, QStringLiteral("tag cp"), path, &start))
            return kIncomplete;
        PathState state = PathState::Missing;
        const Record record = stamped(path, start.tags + origin.tags, &error, &state,
                                      start.row, s.store.loaded(path));
        if (record.path.isEmpty()) {
            fail(error);
            return pathStatus(state);
        }
        s.store.put(record);
        written.append(record);
        mirrorTags(s, record);
    }
    return finish(g, s, written);
}

// `from` and `to` are two different tags, folded: `parseCommand` checks.
int cmdTagMerge(const Globals &g, Session &s, const QString &from, const QString &to)
{
    int status = openSession(&s);
    if (status != kOk)
        return status;

    QVector<Record> written;
    for (const Record &row : s.store.records()) {
        if (!row.tags.contains(from))
            continue;
        // Present only: a gone file has nothing to rewrite and `verify` calls
        // it missing. One nobody may look at, one whose value will not read, one
        // under a root that is away, or one a symlink now leads to, is a mirror
        // not written, said as a note the way `tag add` says one.
        QString why;
        const PathState state = rowState(row.path, &why);
        Starting start{row, row.tags};
        const bool mirrored = state == PathState::Present
            && startingRow(s, QStringLiteral("tag merge"), row.path, &start, &why);
        QStringList next;
        for (const QString &tag : std::as_const(start.tags))
            next.append(tag == from ? to : tag);
        const Record record = retagged(row, next, s.store.loaded(row.path));
        s.store.put(record);
        written.append(record);
        if (mirrored)
            mirrorTags(s, record);
        else if (state != PathState::Missing || !leadsNowhere(row.path))
            warn(g, QStringLiteral("tag merge: %1; its xattr was not written").arg(why));
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
    QString error;
    const QString tag = normalizeTag(rawTags.first(), &error);
    if (tag.isEmpty()) {
        fail(QStringLiteral("has: %1").arg(error));
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveExisting(QStringLiteral("has"), paths, &status);
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
    int status = kOk;
    const QStringList resolved = resolveExisting(QStringLiteral("json"), paths, &status);
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
// the file's own shape, which is the point of `export`. Stored form either
// way: a row under a root stays relative to it, so a dump survives a remount.
int cmdExport(const Globals &g, Session &s)
{
    const int status = openSession(&s);
    if (status != kOk)
        return status;
    if (g.json) {
        QJsonArray rows;
        for (const Record &record : s.store.records())
            rows.append(record.toStoredJson());
        printJson(rows);
        return kOk;
    }
    for (const Record &record : s.store.records()) {
        out() << QString::fromUtf8(
            QJsonDocument(record.toStoredJson()).toJson(QJsonDocument::Compact)) << '\n';
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
        const QJsonDocument doc = jsonDocument(whole, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isArray()) {
            *error = QUtf8StringView(whole).isValidUtf8()
                ? QStringLiteral("import: the dump is not a JSON array: %1")
                      .arg(parse.errorString())
                : QStringLiteral("import: the dump is not valid UTF-8");
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
        QJsonObject row;
        QString why;
        if (!keptObject(line, &row, &why)) {
            *error = !why.isEmpty() ? QStringLiteral("import: line %1: %2").arg(number).arg(why)
                : QUtf8StringView(line).isValidUtf8()
                ? QStringLiteral("import: line %1 is not a JSON object").arg(number)
                : QStringLiteral("import: line %1 is not valid UTF-8: %2")
                      .arg(number).arg(escapedBytes(line));
            return false;
        }
        entries->append({QStringLiteral("line %1").arg(number), row});
    }
    return true;
}

// The bytes a verb reads its input out of: the file named, or stdin where `-`
// names it.
//
// `-` is stdin the way it is for every tool that reads a stream -- `git apply
// -`, `kubectl apply -f -`, `sort -` -- and a file really called `-` is still
// reachable as `./-`. A named fifo is read like a file, so `batch <(...)` works.
//
// Returns kOk, or what the failure costs: a name that is not there is 2, and
// input that is there and will not read -- a directory, a closed stdin -- is
// the 1 every other verb gives for a path nobody may look at. See `pathStatus`.
//
// Bytes, not Qt's text mode, which drops every `\r` -- one inside quotes too.
int readInput(const QString &verb, const QString &source, QByteArray *bytes)
{
    QFile file;
    const bool standard = source == QLatin1String("-");
    if (standard) {
        if (!file.open(STDIN_FILENO, QIODevice::ReadOnly)) {
            fail(QStringLiteral("%1: cannot read stdin: %2").arg(verb, file.errorString()));
            return kIncomplete;
        }
    } else {
        // Asked of the roots first: a file under one that is away is not the
        // file that was meant, whatever answers at its path. Not resolved to a
        // name, which a pipe such as `<(...)` does not have.
        file.setFileName(source);
        QString why;
        PathState opened = pathState(source, &why);
        if (opened == PathState::Present) {
            QString unspelled;
            bool unnameable = false;
            realPath(source, &unspelled, nullptr, &unnameable);
            if (unnameable) {
                fail(QStringLiteral("%1: %2").arg(verb, unspelled));
                return kUsage;
            }
            if (closedStdin(verb, source))
                return kIncomplete;
            opened = openToRead(&file, {}, &why, Reading::Stream);
        }
        // A link that leads nowhere is named hop by hop, as for any operand.
        const QString chain =
            opened == PathState::Missing ? danglingChain(source) : QString();
        if (opened != PathState::Present) {
            fail(chain.isEmpty() ? QStringLiteral("%1: cannot read %2").arg(verb, why)
                                 : QStringLiteral("%1: %2").arg(verb, chain));
            return pathStatus(opened);
        }
    }
    *bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        fail(QStringLiteral("%1: cannot read %2: %3")
                 .arg(verb, standard ? QStringLiteral("stdin") : spelled(source),
                      file.errorString()));
        return kIncomplete;
    }
    return kOk;
}

// A dump's row as the index would hold it, or empty having said why. Its tags
// are folded the way typed ones are -- `Work` is `work`, sorted and each once
// -- and one no folding makes a tag stays as it was, to be refused by name.
// A dump path is a row, so it stands here only where it resolves to itself, as
// `rowState` judges: one a symlink at it or above it leads elsewhere is kept as
// given and Missing, and nothing is written through the link. One that
// resolves to itself takes the spelling its directories store. A path that is
// not here is kept as given, since a dump names paths from other machines; one
// here that resolves to a name no text can spell is refused, as every operand
// is, and so is a file omafiles keeps. `state` and `why` are the row's
// standing here, or the kernel's answer where the path does not resolve.
Record dumpRow(const DumpEntry &entry, const QVector<Guarded> &kept, PathState *state,
               QString *why)
{
    QJsonObject row = entry.row;
    if (const QJsonValue tags = row.value(QStringLiteral("tags")); tags.isArray()) {
        const QJsonArray given = tags.toArray();
        if (std::all_of(given.cbegin(), given.cend(),
                        [](const QJsonValue &tag) { return tag.isString(); })) {
            QStringList folded;
            for (const QJsonValue &tag : given) {
                const QString spelled = normalizeTag(tag.toString(), nullptr);
                folded.append(spelled.isEmpty() ? tag.toString() : spelled);
            }
            row.insert(QStringLiteral("tags"), QJsonArray::fromStringList(uniqueSorted(folded)));
        }
    }
    QString error;
    Record record = keptRow(row, &error);
    if (record.path.isEmpty()) {
        fail(QStringLiteral("import: %1: %2").arg(entry.label, error));
        return {};
    }
    bool unnameable = false;
    const QString resolved = realPath(record.path, why, state, &unnameable);
    if (unnameable) {
        fail(QStringLiteral("import: %1: %2").arg(entry.label, *why));
        return {};
    }
    if (resolved.isEmpty())
        return record;
    if (const PathState given = rowState(record.path, why); given != PathState::Present) {
        *state = given;
        return record;
    }
    // Only a spelling apart from the path given, which resolves to itself.
    record.path = resolved;
    *state = storageState(resolved, why);
    if (*state == PathState::Present) {
        int status = kOk;
        refuseKept(QStringLiteral("import: %1").arg(entry.label), {resolved}, &status, false,
                   kept);
        if (status != kOk)
            return {};
    }
    return record;
}

// A merge, not a replacement: an import that silently dropped the rows it did
// not mention would make restoring one machine's tags onto another a way of
// losing the second machine's. One argument: `parseCommand` checks.
int cmdImport(const Globals &g, Session &s, const QString &source)
{
    // So `omafiles export | ssh box omafiles import -` works.
    QByteArray input;
    int status = readInput(QStringLiteral("import"), source, &input);
    if (status != kOk)
        return status;

    status = openSession(&s);
    if (status != kOk)
        return status;

    QVector<DumpEntry> entries;
    QString why;
    // Bad input, not a broken store: exit 3 is what a caller reads as "the
    // index could not be written".
    if (!readDump(withoutBom(input), &entries, &why)) {
        fail(why);
        return kUsage;
    }
    // A bad row returns before the commit, so it merges nothing.
    QVector<Record> written;
    // Said once however many lines name the path, and only once no line refused.
    QStringList unmirrored;
    const QVector<Guarded> kept = keptFiles();
    for (const DumpEntry &entry : std::as_const(entries)) {
        // Only ENOENT is not here: a path the kernel answers anything else for
        // is one it will not describe, a loop of links included.
        QString unwritten;
        PathState state = PathState::Missing;
        Record record = dumpRow(entry, kept, &state, &unwritten);
        if (record.path.isEmpty())
            return kUsage;
        // Present only, the same deliberate answer `tag merge` gives: the
        // question is whether there is a file here to stamp, not whether the
        // row is true. A dump from another machine names paths that are not on
        // this one at all -- that is the point of merging one -- so a missing
        // one is silent. One the system will not describe, under a root that
        // is away, or that a symlink now leads through, is a mirror not
        // attempted, and said the way `tag add` says one not written. A file
        // that is here starts from its own tags.
        Starting start{s.store.get(record.path), s.store.get(record.path).tags};
        if (state == PathState::Present
            && !startingRow(s, QStringLiteral("import"), record.path, &start))
            return kIncomplete;
        record.tags = uniqueSorted(start.tags + record.tags);
        // A merge adds; it does not un-know things. A dump line with no
        // `tagged` field must not take the stamp off the row it merges into, or
        // `--since` can never judge that file again.
        record.tagged = qMax(record.tagged, start.row.tagged);
        s.store.put(record);
        written.append(record);
        if (state == PathState::Present)
            mirrorTags(s, record);
        else if ((state != PathState::Missing || !leadsNowhere(record.path))
                 && !unmirrored.contains(unwritten))
            unmirrored.append(unwritten);
    }
    for (const QString &unwritten : std::as_const(unmirrored))
        warn(g, QStringLiteral("import: %1; its own copy of the tags was not written")
                    .arg(unwritten));
    return finish(g, s, committedRows(s.store, written));
}

// The row at `dir`, and every row inside it where `folder`.
QStringList rowsAt(const Store &store, const QString &dir, bool folder)
{
    QStringList paths;
    for (const Record &record : store.records()) {
        if (record.path == dir || (folder && isUnderDirectory(record.path, dir)))
            paths.append(record.path);
    }
    return paths;
}

// `rowsAt`, where `folder` says a directory is at `dir`, for `rm`, `trash` and
// `mv`, and always for a destination, which is free: every row at it or under
// it is one whose file is gone, whatever lands there. False, with
// the reason, where a root declared at `dir` or inside it is not here or cannot
// be checked: the declaration wins over the disk, so that root's rows are not
// the tree's to act on, and the operand is refused.
bool rowsUnder(const Store &store, const QString &dir, bool folder, QStringList *paths,
               QString *why)
{
    for (const Root &root : roots()) {
        QString said;
        if (!isUnderDirectory(root.path, dir)
            || storageState(root.path, &said) == PathState::Present)
            continue;
        // Held only where the root's directory is there to be held.
        *why = root.path == dir ? said
            : leadsNowhere(root.path)
            ? QStringLiteral("%1 is on the way to %2").arg(spelled(dir), said)
            : QStringLiteral("%1 holds %2").arg(spelled(dir), said);
        return false;
    }
    *paths = rowsAt(store, dir, folder);
    return true;
}

// Whether `path` itself, a link not followed, is a directory.
bool isFolder(const QString &path)
{
    struct stat st {};
    return statState(path, nullptr, false, &st) == PathState::Present && S_ISDIR(st.st_mode);
}

// Why nothing may be written at that path, or empty where the path is free.
//
// `lstat`, because a dangling link is a thing at that path whether or not what
// it points at is; coreutils will not write through one either -- `cp` says
// "not writing through dangling symlink" in so many words. And only ENOENT is
// free: a destination nobody may look at is not one known to be empty.
QString occupied(const QString &verb, const QString &path)
{
    QString why;
    switch (statState(path, &why, false)) {
    case PathState::Missing:
        return {};
    case PathState::Present:
        return QStringLiteral("%1: %2 already exists").arg(verb, spelled(path));
    default:
        return QStringLiteral("%1: %2").arg(verb, why);
    }
}

// Whether `to` is `from`, resolved, spelled in another case on a directory
// that folds it: a rename of the case alone, as Darwin and Windows allow one.
bool recased(const QString &from, const QString &to)
{
    return to != from && canonicalLinkPath(to, nullptr) == from;
}

// `rowsUnder` for `to`, the free destination of `from`. On a directory that
// folds case a row spelled in another case names `to` as well -- an operand's
// rule -- so it is as stale, unless it is `from`'s own, a rename of the case.
bool staleUnder(const Store &store, const QString &from, const QString &to, QStringList *paths,
                QString *why)
{
    if (!rowsUnder(store, to, true, paths, why))
        return false;
    QString name;
    const QString dir = typedDirectory(to, &name);
    QStringList folded;
    for (const Record &record : store.records()) {
        QString at = record.path;
        if (at == dir || !isUnderDirectory(at, dir) || isUnderDirectory(at, from))
            continue;
        while (typedDirectory(at, nullptr) != dir)
            at = typedDirectory(at, nullptr);
        QString entry;
        typedDirectory(at, &entry);
        if (entry != name && entry.compare(name, Qt::CaseInsensitive) == 0)
            folded.append(record.path);
    }
    if (!folded.isEmpty() && foldsCase(dir))
        paths->append(folded);
    return true;
}

// Why the destination's parent cannot hold a file, or empty if it can.
//
// The system's own reason where it has one: `QFileInfo::exists` is false for a
// directory nobody may look at as well as for one that is not there, so a bare
// `strerror(ENOENT)` about a chmod'd parent sends the reader after the wrong
// thing. The reason names the parent, because the destination beside it does
// not say which of its components is the one that will not answer; a link that
// leads nowhere is named hop by hop, as for any operand.
QString unusableParent(const QString &parent)
{
    QString detail;
    const PathState state = pathState(parent, &detail);
    if (state == PathState::Unknown || state == PathState::Unreachable)
        return detail;
    if (state == PathState::Missing) {
        const QString chain = danglingChain(parent);
        return chain.isEmpty() ? systemReason(ENOENT) : chain;
    }
    return QFileInfo(parent).isDir() ? QString()
                                     : systemReason(ENOTDIR);
}

// Where `mv` or `cp` puts `from` when told `typed`: inside it where it is a
// directory, at it otherwise. The directory part is resolved by the kernel as it
// was typed -- `L/..` is the parent of where `L` leads, POSIX 4.13 -- and never
// cleaned as text. Empty, and said, where no file can go there.
QString destination(const QString &doing, const QString &from, const QString &typed)
{
    // No name at all, which `rename(2)` answers ENOENT for.
    if (typed.isEmpty()) {
        fail(QStringLiteral("%1 %2 to %3: %4")
                 .arg(doing, spelled(from), spelled(typed), systemReason(ENOENT)));
        return {};
    }
    // Under a root that is away, whatever the disk says of the path: udisks
    // removes an unplugged disk's mount point, and a trailing `/` or a missing
    // parent would otherwise be the answer.
    if (QString away; pathState(typed, &away) == PathState::Unreachable) {
        fail(QStringLiteral("%1 %2 to %3: %4").arg(doing, spelled(from), spelled(typed), away));
        return {};
    }
    struct stat st {};
    QString name = QFileInfo(from).fileName();
    QString typedName;
    const QString typedIn = typedDirectory(typed, &typedName);
    // A directory that is the source spelled in another case is the new name.
    // Asked of the entry, since a trailing `/` follows a link to the source.
    const bool into = statState(typed, nullptr, true, &st) == PathState::Present
        && S_ISDIR(st.st_mode)
        && (typedName == name
            || canonicalLinkPath(QDir(typedIn).filePath(typedName), nullptr) != from);
    // A trailing `/` names a directory, so `rename(2)` refuses it to anything
    // else with ENOTDIR, and so does coreutils.
    if (!into && typed.endsWith(QLatin1Char('/'))
        && (statState(from, nullptr, false, &st) != PathState::Present || !S_ISDIR(st.st_mode))) {
        fail(QStringLiteral("%1 %2 to %3: %4")
                 .arg(doing, spelled(from), spelled(typed), systemReason(ENOTDIR)));
        return {};
    }
    const QString folder = into ? typed : typedDirectory(typed, &name);
    QString why;
    const QString parent = canonicalPath(folder, &why);
    QString unusable = unusableParent(parent.isEmpty() ? folder : parent);
    if (unusable.isEmpty() && parent.isEmpty())
        unusable = why;
    const QString to = QDir(parent.isEmpty() ? folder : parent).filePath(name);
    if (unusable.isEmpty())
        return to;
    fail(QStringLiteral("%1 %2 to %3: %4").arg(doing, spelled(from), spelled(to), unusable));
    return {};
}

// Whether a rename can carry `from` to `to`.
//
// `rename(2)` answers EXDEV where the two directories are on different mounts,
// two mounts of one filesystem included, so it is the mount, statx's
// `STATX_MNT_ID`, that decides, not `st_dev`. Where either cannot be read it says
// yes, so a dry run never invents a refusal it cannot justify -- the real run
// is there to give the system's own reason.
bool sameMount(const QString &from, const QString &to)
{
    const auto mount = [](const QString &path, struct statx *into) {
        return ::statx(AT_FDCWD, QFile::encodeName(typedDirectory(path, nullptr)).constData(),
                       0, STATX_MNT_ID, into) == 0
            && (into->stx_mask & STATX_MNT_ID);
    };
    struct statx one {};
    struct statx other {};
    return !mount(from, &one) || !mount(to, &other) || one.stx_mnt_id == other.stx_mnt_id;
}

// Copies the bytes of the regular file `from` into `to`, a file this makes,
// then its own `user.omafiles.tags` verbatim, as a rename would carry it, and
// then its mode.
//
// In that order, on a file only this run can open: a `user.*` write needs
// write permission on the file, so a copy made read-only first refuses its own
// tags. The mode is the source's for a move, and for a copy what coreutils `cp`
// gives without `-p` -- the source's, less the umask, less setuid, setgid and
// sticky.
// A value the source will not show fails the copy. One the copy refuses is
// `note` where `recorded`, the row that lands, holds every word of it, the rule
// `tag add` keeps, and fails the copy where the value is the only record of a
// word. Where either end cannot hold a value at all, the row is the record:
// the copy takes the row's tags from a source without one, and a copy without
// one is not a refusal unless the value holds a word the row does not.
bool copyFile(const QString &from, const QString &to, bool move, const QStringList &recorded,
              QString *error, QString *note)
{
    const auto said = [error](int failure) {
        *error = systemReason(failure);
        return false;
    };
    // NONBLOCK so a fifo swapped in since the caller looked is refused, not
    // waited on.
    const int in = ::open(QFile::encodeName(from).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (in < 0)
        return said(errno);
    struct stat st {};
    if (::fstat(in, &st) != 0) {
        const int failure = errno;
        ::close(in);
        return said(failure);
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(in);
        *error = QStringLiteral("%1 is not a regular file").arg(spelled(from));
        return false;
    }
    QByteArray value;
    QString why;
    const TagsValue held = readTagsValue(in, &value, &why);
    if (held == TagsValue::Unknown) {
        ::close(in);
        *error = QStringLiteral("the tags it carries cannot be read: %1").arg(why);
        return false;
    }
    if (held == TagsValue::Unsupported)
        value = recorded.join(QLatin1Char(',')).toUtf8();
    const QByteArray target = QFile::encodeName(to);
    const int out = ::open(target.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int failure = out < 0 ? errno : 0;
    // 0600 whatever the umask says, until the tags are on.
    if (out >= 0 && ::fchmod(out, 0600) != 0)
        failure = errno;
    char buffer[65536];
    while (failure == 0) {
        const ssize_t got = ::read(in, buffer, sizeof buffer);
        if (got == 0)
            break;
        if (got < 0) {
            if (errno != EINTR)
                failure = errno;
            continue;
        }
        for (ssize_t put = 0; put < got && failure == 0;) {
            const ssize_t wrote = ::write(out, buffer + put, static_cast<size_t>(got - put));
            if (wrote >= 0)
                put += wrote;
            else if (errno != EINTR)
                failure = errno;
        }
    }
    ::close(in);
    QStringList unrecorded;
    QStringList tags = salvagedTags(tagsOfValue(value), &unrecorded);
    tags.removeIf([&recorded](const QString &tag) { return recorded.contains(tag); });
    unrecorded += tags;
    QByteArray none;
    if (failure == 0 && !value.isEmpty()
        && (!unrecorded.isEmpty() || readTagsValue(out, &none, nullptr) != TagsValue::Unsupported)
        && !writeTagsValue(out, value, &why)) {
        if (!unrecorded.isEmpty()) {
            *error = QStringLiteral("the copy refused %1, which no row records: %2")
                         .arg(recorded.isEmpty() ? QStringLiteral("the tags")
                                                 : QStringLiteral("the words (%1)")
                                                       .arg(saidWords(unrecorded)),
                              why);
            ::close(out);
            ::unlink(target.constData());
            return false;
        }
        *note = QStringLiteral("xattr set failed on %1: %2").arg(spelled(to), why);
    }
    if (failure == 0) {
        mode_t mask = ::umask(0);
        ::umask(mask);
        const mode_t mode = move ? st.st_mode & 07777 : st.st_mode & 0777 & ~mask;
        if (::fchmod(out, mode) != 0)
            failure = errno;
    }
    if (out >= 0 && ::close(out) != 0 && failure == 0)
        failure = errno;
    if (failure == 0)
        return true;
    if (out >= 0)
        ::unlink(target.constData());
    return said(failure);
}

// Moves the path as named, link and all. See `resolveNamed`.
//
// `rename(2)` is the whole move within one filesystem; across one it answers
// EXDEV. A file then crosses as `copyFile` + `unlink`, and a symlink as
// `readlink` + `symlink` + `unlink`, because a copy would read through the
// link. A directory never reaches here; `cmdMove` refuses it. `recorded` is
// the tags its row lands with: see `copyFile`.
//
// A rename of a file onto another name of itself "shall perform no other
// action" (POSIX rename(2)), so a change of case alone goes by a free name
// beside it, the two-step rename used on every case-insensitive filesystem.
bool moveNamed(const QString &from, const QString &to, const QStringList &recorded,
               QString *error, QString *note)
{
    const QByteArray source = QFile::encodeName(from);
    const QByteArray destination = QFile::encodeName(to);
    if (recased(from, to)) {
        const QByteArray aside = source + ".omafiles~" + QByteArray::number(::getpid());
        struct stat st {};
        int failure = ::lstat(aside.constData(), &st) == 0 ? EEXIST : 0;
        if (failure == 0 && ::rename(source.constData(), aside.constData()) != 0)
            failure = errno;
        if (failure == 0 && ::rename(aside.constData(), destination.constData()) != 0) {
            failure = errno;
            ::rename(aside.constData(), source.constData());
        }
        if (failure != 0 && error)
            *error = systemReason(failure);
        return failure == 0;
    }
    if (::rename(source.constData(), destination.constData()) == 0)
        return true;
    // Kept before building any message can overwrite it: the reason is the
    // system's to give.
    int failure = errno;
    if (failure != EXDEV) {
        if (error)
            *error = systemReason(failure);
        return false;
    }
    if (!QFileInfo(from).isSymLink()) {
        if (!copyFile(from, to, true, recorded, error, note))
            return false;
        if (::unlink(source.constData()) == 0)
            return true;
        // The file is at both ends and the source will not go: take the copy
        // back rather than leave the move half done.
        failure = errno;
        ::unlink(destination.constData());
    } else {
        const QByteArray target = linkTarget(source);
        if (!target.isEmpty()
            && ::symlink(target.constData(), destination.constData()) == 0) {
            if (::unlink(source.constData()) == 0)
                return true;
            failure = errno;
            ::unlink(destination.constData());
        } else {
            failure = errno;
        }
    }
    if (error)
        *error = systemReason(failure);
    return false;
}

// A move whose rows the index would not take, put back where its rows are.
void undoMove(const QString &from, const QString &to, const QStringList &recorded)
{
    QString why;
    QString note;
    if (moveNamed(to, from, recorded, &why, &note))
        fail(QStringLiteral("mv: %1 was moved back to %2").arg(spelled(to), spelled(from)));
    else
        fail(QStringLiteral("mv: %1 could not be moved back to %2: %3")
                 .arg(spelled(to), spelled(from), why));
}

// Where a row filed under `was` lands once `from` has become `to`.
QString movedPath(const QString &was, const QString &from, const QString &to)
{
    return was == from ? to : QDir(to).filePath(relativeTo(was, from));
}

// The one shape `mv` and `cp` answer in, in both modes: where the file went is
// the thing every consumer needs, so it is always there. `rm` and `trash` say
// `{"removed":[...]}` the same way.
QJsonObject movedJson(const QString &from, const QString &to, const QVector<Record> &rows)
{
    return QJsonObject{{QStringLiteral("from"), from},
                       {QStringLiteral("to"), to},
                       {QStringLiteral("rows"), rowsJson(rows)}};
}

// The tags the row of `from` lands with at the far end of `mv` or `cp`, asked
// before the file goes: a row united with the valid tags the value carries, the
// `startingRow` rule, so a value a filesystem with no `user.*` cannot hold loses
// no tag. No row is still none, and a link keeps the row alone, as it holds no
// value. False, with why in `unread`, where the value will not show.
bool landingTags(const Session &s, const QString &from, QStringList *tags, QString *unread)
{
    Starting start;
    *tags = s.store.get(from).tags;
    if (QFileInfo(from).isSymLink())
        return true;
    if (!startingRow(s, QString(), from, &start, unread)) {
        *unread = QStringLiteral("the tags it carries cannot be read: %1").arg(*unread);
        return false;
    }
    if (!start.row.path.isEmpty())
        *tags = start.tags;
    return true;
}

// The rows `rows`, under `from`, as they land once `from` has become `to`,
// `from`'s own holding `landing`, restamped where that adds a tag.
//
// A row under a folder is the row that was there under its new name: the
// rename carried the file, so the tags and the stamp are already the answer,
// and the path is a string rewrite. Nothing asks the system about those, which
// is why every row follows even through a directory nobody may look inside.
QVector<Record> movedRows(const Store &store, const QStringList &rows, const QString &from,
                          const QString &to, const QStringList &landing)
{
    QVector<Record> moved;
    for (const QString &was : rows) {
        Record record = was == from ? retagged(store.get(was), landing) : store.get(was);
        record.path = movedPath(was, from, to);
        moved.append(record);
    }
    return moved;
}

// Follows a move with the index: every row under the folder, not just the row
// for the folder itself.
void followMove(Session &s, const QStringList &rows, const QString &from, const QString &to,
                const QStringList &landing, QVector<Record> *written)
{
    const QVector<Record> moved = movedRows(s.store, rows, from, to, landing);
    for (const QString &was : rows)
        s.store.remove(was);
    for (const Record &record : moved) {
        s.store.put(record);
        written->append(record);
    }
}

// A destination is free, so a row at it or under it is a row whose file is
// gone. Landing there drops it, named with the tags it held, rather than
// handing those tags to whatever arrives; `how` names the act that drops it.
// Said as the rule, so it stays true of a run that then fails and of a dry run.
void dropStale(const Globals &g, Session &s, const QString &verb, const QStringList &rows,
               const QString &how = QStringLiteral("landing there"))
{
    for (const QString &stale : rows) {
        warn(g, QStringLiteral("%1: %2 had a row for a file that is gone; %3 %4 it and its "
                               "tags %5")
                    .arg(verb, spelled(stale), how,
                         g.dryRun ? QStringLiteral("would drop") : QStringLiteral("drops"),
                         s.store.get(stale).tags.join(QLatin1Char(','))));
        s.store.remove(stale);
    }
}

// Whether a row is at `from`'s own name where that is a link now, which leads
// to some other file: it is of a file that is gone. Asked before the move.
bool rowAtLink(const Session &s, const QString &from)
{
    return s.store.contains(from) && rowState(from, nullptr) == PathState::Missing;
}

// Moving the link drops that row.
void dropLinkRow(const Globals &g, Session &s, const QString &from)
{
    dropStale(g, s, QStringLiteral("mv"), {from}, QStringLiteral("moving the link"));
}

// `mv` and `trash` leave the directory of a root that is here where it is
// declared: moving one is `roots rm` and `roots add`, the verbs that keep the
// declarations. One that is away is `rowsUnder`'s refusal.
void refuseRoots(const QString &verb, const QStringList &paths, int *status)
{
    QVector<Guarded> declared;
    QHash<QString, QString> ids;
    for (const Root &root : roots()) {
        if (storageState(root.path, nullptr) != PathState::Present)
            continue;
        declared.append(guarded(root.path));
        ids.insert(root.path, root.id);
    }
    refuseGuarded(verb, paths, declared, [&ids](const QString &path, bool holds) {
        const QString id = ids.value(path);
        return (holds ? QStringLiteral("it holds root %1 at %2").arg(id, spelled(path))
                      : QStringLiteral("it is root %1").arg(id))
            + QStringLiteral("; `omafiles roots rm --id %1` first").arg(id);
    }, status);
}

// Names each operand of `rm`, `trash` or `mv` that no rule of theirs makes the
// path meant, into `status`: a last component `.` or `..`, which POSIX `rm`
// refuses, `/`, which coreutils refuses with `--preserve-root`, and what
// `refuseKept` refuses.
void refuseOperands(const QString &verb, const QStringList &typed,
                    const QStringList &resolved, int *status)
{
    bool refused = false;
    for (const QString &path : typed) {
        QString name;
        typedDirectory(path, &name);
        if (name == QLatin1String(".") || name == QLatin1String("..")) {
            fail(QStringLiteral("%1: refusing %2: it ends in `.` or `..`, which POSIX rm "
                                "refuses; name the directory itself").arg(verb, spelled(path)));
            refused = true;
        }
    }
    if (resolved.contains(QStringLiteral("/"))) {
        fail(QStringLiteral("%1: / is the whole filesystem; it is refused, as coreutils "
                            "--preserve-root refuses it").arg(verb));
        refused = true;
    }
    if (refused && *status == kOk)
        *status = kUsage;
    refuseKept(verb, resolved, status);
}

// Why `from` cannot move to `to`, or empty where it can, in a dry run too. A
// mount point is refused, as rename(2) answers EBUSY for one. A rename is the
// whole move within one filesystem, and across one a regular file is copied and
// a symlink recreated, while a directory and a fifo, a socket or a device are
// refused.
QString cannotMove(const QString &from, const QString &to)
{
    struct statx own {};
    if (::statx(AT_FDCWD, QFile::encodeName(from).constData(), AT_SYMLINK_NOFOLLOW, 0, &own) == 0
        && (own.stx_attributes & own.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT)) {
        return QStringLiteral("mv: cannot move %1 to %2: %3; it is a mount point")
            .arg(spelled(from), spelled(to), systemReason(EBUSY));
    }
    struct stat st {};
    if (statState(from, nullptr, false, &st) != PathState::Present || S_ISLNK(st.st_mode)
        || S_ISREG(st.st_mode) || sameMount(from, to))
        return {};
    if (S_ISDIR(st.st_mode)) {
        return QStringLiteral("mv: cannot move %1 to %2: %3; a folder cannot cross "
                              "filesystems -- move it another way, then `omafiles "
                              "reindex` and `omafiles prune`")
            .arg(spelled(from), spelled(to), systemReason(EXDEV));
    }
    return QStringLiteral("mv: cannot move %1 to %2: %1 is not a regular file")
        .arg(spelled(from), spelled(to));
}

int cmdMove(const Globals &g, Session &s, const QStringList &positionals)
{
    int status = kOk;
    // The path that was named, not the one it points at: a rename moves the
    // link. See `resolveNamed`. The destination keeps the link's own name for
    // the same reason -- `mv link.txt dest/` lands `dest/link.txt`.
    const QStringList source = resolveNamed(QStringLiteral("mv"), {positionals.at(0)}, &status);
    refuseOperands(QStringLiteral("mv"), {positionals.at(0)}, source, &status);
    refuseRoots(QStringLiteral("mv"), source, &status);
    if (status != kOk)
        return status;
    const QString from = source.first();
    // Resolved through its parent, because the destination does not exist yet:
    // a canonical source compared against an unresolved destination lets a
    // symlink walk straight past the inside-itself guard below. A parent that
    // cannot hold it is refused in both modes, like the already-exists and
    // cross-filesystem refusals: it is a fact about the path, there to be read.
    QString to = destination(QStringLiteral("mv: cannot move"), from, positionals.at(1));
    if (to.isEmpty())
        return kUsage;
    refuseKept(QStringLiteral("mv"), {to}, &status);
    if (status != kOk)
        return status;

    if (const QString taken = occupied(QStringLiteral("mv"), to);
        !taken.isEmpty() && !recased(from, to)) {
        fail(taken);
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
        fail(QStringLiteral("mv: %1 is inside %2").arg(spelled(to), spelled(from)));
        return kUsage;
    }
    // The source's mount against the destination's parent is the comparison
    // `rename(2)` makes, the parent because the destination does not exist yet.
    if (const QString refusal = cannotMove(from, to); !refusal.isEmpty()) {
        fail(refusal);
        return kUsage;
    }
    QStringList moving;
    QStringList stale;
    QString away;
    const bool folder = isFolder(from);
    if (!rowsUnder(s.store, from, folder, &moving, &away)
        || !staleUnder(s.store, from, to, &stale, &away)) {
        fail(QStringLiteral("mv: %1").arg(away));
        return kUsage;
    }
    QStringList tags;
    QString unread;
    if (!landingTags(s, from, &tags, &unread)) {
        fail(QStringLiteral("mv: cannot move %1 to %2: %3")
                 .arg(spelled(from), spelled(to), unread));
        return kUsage;
    }
    if (rowAtLink(s, from)) {
        moving.removeOne(from);
        dropLinkRow(g, s, from);
    }
    dropStale(g, s, QStringLiteral("mv"), stale);

    if (g.dryRun) {
        // In the shape asked for: `--json --dry-run` must not hand a JSON
        // consumer an arrow between two paths.
        if (g.json) {
            printJson(movedJson(from, to, movedRows(s.store, moving, from, to, tags)));
        } else {
            // The destination, which is what the real run prints: a preview
            // whose shape differs from the result is a preview nothing can be
            // tested against.
            out() << to << '\n';
        }
        return kOk;
    }
    QString refused;
    QString note;
    if (!moveNamed(from, to, tags, &refused, &note)) {
        // The reason is the system's to give: one guess for every failure
        // sends the reader after the wrong thing.
        fail(QStringLiteral("mv: cannot move %1 to %2: %3")
                 .arg(spelled(from), spelled(to), refused));
        return kUsage;
    }
    if (!note.isEmpty())
        warn(g, QStringLiteral("mv: %1").arg(note));
    // As the directory stores the name it made, where it drops or folds
    // something typed: `z4.` is `z4` on vfat.
    to = storedSpelling(to);

    QVector<Record> written;
    followMove(s, moving, from, to, tags, &written);

    // Announced here and not by the commit, because this verb has its own shape
    // to print and two documents on one stdout is not an answer. Printed after
    // the commit for the reason `rm` is: a destination reported before the row
    // that records it has landed is a claim the next run may contradict.
    const int done = finish(g, s, written, false);
    if (done != kOk) {
        undoMove(from, to, tags);
        return done;
    }
    if (g.json)
        printJson(movedJson(from, to, written));
    else
        out() << to << '\n';
    // The rename happened and every row followed it, so there is nothing left
    // for this to be short of. A file's own copy the filesystem refused is
    // `tag add`'s rule and not this one: named on stderr, exit 0, because the
    // row is the record.
    return kOk;
}

int cmdCopy(const Globals &g, Session &s, const QStringList &positionals)
{
    int status = kOk;
    // The path that was named. `cp` reads through a link -- POSIX says an
    // unadorned `cp` follows the source, and coreutils writes a plain file with
    // the target's bytes -- but it files the copy under the name it was handed:
    // `cp link.txt dest/` lands `dest/link.txt`, not the target's name.
    const QStringList source = resolveNamed(QStringLiteral("cp"), {positionals.at(0)}, &status);
    if (status != kOk)
        return status;
    const QString from = source.first();

    // Named as a link, read through it. `resolveNamed` files the copy under
    // the name it was handed and is happy with a link that points nowhere,
    // because for `rm`, `trash` and `mv` a dangling link is a path that is
    // there -- but `cp` reads the bytes at the far end, and there are none.
    // That is the target missing and exit 2, as it is for every verb that
    // reads through a link. The tags that travel with a copy are the tags of
    // the file that was copied, filed under that fully resolved path.
    const QStringList through = resolveExisting(QStringLiteral("cp"), {from}, &status);
    if (status != kOk)
        return status;
    const QString reached = through.first();

    // Before the dry run prints a copy, not in the failure of the copy, which
    // a dry run never reaches. Nothing about a directory, a fifo or a device
    // changes between the two runs, and reading a fifo waits for a writer.
    struct stat kind {};
    statState(reached, nullptr, true, &kind);
    if (!S_ISREG(kind.st_mode)) {
        fail(S_ISDIR(kind.st_mode)
                 ? QStringLiteral("cp: %1 is a directory; cp takes files").arg(spelled(from))
                 : QStringLiteral("cp: %1 is not a regular file; cp takes files")
                       .arg(spelled(from)));
        return kUsage;
    }

    // Resolved through its parent, like `mv`'s; a parent that cannot hold it is
    // refused in both modes, like the already-exists refusal.
    QString to = destination(QStringLiteral("cp: cannot copy"), from, positionals.at(1));
    if (to.isEmpty())
        return kUsage;
    // The file at the far end of a link is the one read, so it is asked too.
    refuseKept(QStringLiteral("cp"), reached == from ? QStringList{from, to}
                                                     : QStringList{from, reached, to},
               &status);
    if (status != kOk)
        return status;

    if (const QString taken = occupied(QStringLiteral("cp"), to); !taken.isEmpty()) {
        fail(taken);
        return kUsage;
    }

    if ((status = openSession(&s)) != kOk)
        return status;
    QStringList stale;
    QString away;
    if (!staleUnder(s.store, from, to, &stale, &away)) {
        fail(QStringLiteral("cp: %1").arg(away));
        return kUsage;
    }
    QStringList tags;
    QString unread;
    if (!landingTags(s, reached, &tags, &unread)) {
        fail(QStringLiteral("cp: cannot copy %1 to %2: %3")
                 .arg(spelled(from), spelled(to), unread));
        return kUsage;
    }
    dropStale(g, s, QStringLiteral("cp"), stale);

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
    QString refused;
    QString note;
    if (!copyFile(from, to, false, tags, &refused, &note)) {
        fail(QStringLiteral("cp: cannot copy %1 to %2: %3")
                 .arg(spelled(from), spelled(to), refused));
        return kUsage;
    }
    if (!note.isEmpty())
        warn(g, QStringLiteral("cp: %1").arg(note));
    to = storedSpelling(to);

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
    }
    // The commit stays quiet; this verb prints its own shape, after the row it
    // reports is certainly on disk. See `cmdMove`. The destination is the
    // answer, and it is not printed when the answer is asked for as JSON: two
    // things on stdout is not a document.
    const int done = finish(g, s, written, false);
    if (done != kOk) {
        // The copy goes with the row the index would not take.
        if (::unlink(QFile::encodeName(to).constData()) == 0)
            fail(QStringLiteral("cp: %1 was removed again").arg(spelled(to)));
        else
            fail(QStringLiteral("cp: %1 could not be removed again: %2")
                     .arg(spelled(to), systemReason(errno)));
        return done;
    }
    if (g.json)
        printJson(movedJson(from, to, written));
    else
        out() << to << '\n';
    return kOk;
}

// Why no `gio` will run. One that is on PATH and may not be run is named: "not
// installed" would send somebody to install what they have.
QString noGio()
{
    for (const QString &dir : executableDirs()) {
        const QString candidate = QDir(dir).filePath(QStringLiteral("gio"));
        if (statState(candidate, nullptr) == PathState::Present
            && ::access(QFile::encodeName(candidate).constData(), X_OK) != 0) {
            return QStringLiteral("trash: %1 cannot be run: %2")
                .arg(spelled(candidate), systemReason(errno));
        }
    }
    return QStringLiteral("trash: gio is not installed; it comes with glib2 and is what "
                          "puts a file in the desktop's trash");
}

int cmdRemove(const Globals &g, Session &s, const QStringList &paths, bool toTrash)
{
    const QString verb = toTrash ? QStringLiteral("trash") : QStringLiteral("rm");
    int status = kOk;
    // The path that was named, not the one it points at. See `resolveNamed`.
    // The refusal waits until every check below has spoken, so a mixed list
    // names each operand for its own reason.
    const QStringList named = resolveNamed(verb, paths, &status);
    refuseOperands(verb, paths, named, &status);
    if (toTrash)
        refuseRoots(verb, named, &status);
    // An operand inside another is that one's, the way two spellings of one
    // path are one operand: `trash z z/y/f` is `trash z`.
    QStringList resolved;
    for (const QString &path : named) {
        if (std::none_of(named.cbegin(), named.cend(), [&path](const QString &outer) {
                return outer != path && isUnderDirectory(path, outer);
            }))
            resolved.append(path);
    }

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
    const QString gioProgram = toTrash ? findExecutable(QStringLiteral("gio")) : QString();
    if (toTrash && gioProgram.isEmpty()) {
        fail(noGio());
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
                fail(QStringLiteral("rm: %1 is a directory; rm takes files").arg(spelled(path)));
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
    QMap<QString, QStringList> under;
    int rows = 0;
    for (const QString &path : resolved) {
        QString away;
        if (!rowsUnder(s.store, path, isFolder(path), &under[path], &away)) {
            fail(QStringLiteral("%1: %2").arg(verb, away));
            status = kUsage;
        }
        rows += qMax(1, under.value(path).size());
    }
    if (status != kOk)
        return status;
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
                gio.start(gioProgram, {QStringLiteral("trash"), path});
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
                        fail(noGio());
                    } else {
                        fail(detail.isEmpty()
                                 ? QStringLiteral("trash: gio trash failed on %1")
                                       .arg(spelled(path))
                                 : QStringLiteral("trash: %1").arg(detail));
                    }
                    refused = true;
                    continue;
                }
            } else if (!QFile::remove(path)) {
                fail(QStringLiteral("rm: cannot remove %1: %2")
                         .arg(spelled(path), systemReason(errno)));
                refused = true;
                continue;
            }
        }
        // A folder handed to the trash takes everything under it, and so must
        // its rows -- otherwise the index keeps tags for files that are in the
        // wastebasket. The operand first, then its rows, sorted.
        gone.append(path);
        for (const QString &row : under.value(path)) {
            if (row != path)
                gone.append(row);
        }
    }
    gone.removeDuplicates();
    // The rows of everything that did go, whether or not something else did
    // not: a removal that failed halfway has still removed things, and the
    // index quietly keeping rows for files that are no longer there is the
    // exact state `prune` exists to clean up after.
    status = dropRows(g, &s.store, gone, &done);
    if (status != kOk) {
        // A removal cannot be taken back, so the rows stay for `prune`.
        if (!g.dryRun && !gone.isEmpty())
            fail(QStringLiteral("%1: the files named went, and their rows stay until "
                                "`omafiles prune` drops them").arg(verb));
        return status;
    }
    if (g.json)
        printJson(QJsonObject{{QStringLiteral("removed"), done}});
    return refused ? kUsage : kOk;
}

// ------------------------------------------------------------------- chmod

// One `who op perms` step of a mode. Octal is the single step `a=<bits>`.
struct ModeStep {
    mode_t who = 0;      // the rwx bits of the classes named: u 0700, g 0070, o 0007
    char op = '=';
    mode_t perms = 0;    // r 0444, w 0222, x 0111, before `who` narrows them
};

// `644`, `0755`, or clauses like `u+x,go-w`: coreutils' grammar, less `X`, the
// `u`/`g`/`o` copy forms, and the umask reading of a clause with no class.
bool parseMode(const QString &text, QVector<ModeStep> *steps, QString *error)
{
    const auto special = [&] {
        *error = QStringLiteral("chmod: %1 asks for setuid, setgid or sticky, which this "
                                "program never sets or clears")
                     .arg(spelled(text));
        return false;
    };
    const auto invalid = [&] {
        *error = QStringLiteral("chmod: invalid mode: %1; give octal like 644, or "
                                "clauses like u+x,go-w over u, g, o and a")
                     .arg(spelled(text));
        return false;
    };
    // Leading zeros past the fourth digit are taken, as coreutils takes them.
    static const QRegularExpression octal(
        QRegularExpression::anchoredPattern(QStringLiteral("0*[0-7]{1,4}")));
    if (octal.match(text).hasMatch()) {
        const mode_t value = text.toUInt(nullptr, 8);
        if (value & 07000)
            return special();
        steps->append({0777, '=', value});
        return true;
    }
    for (const QString &clause : text.split(QLatin1Char(','))) {
        int at = 0;
        mode_t who = 0;
        for (; at < clause.size(); ++at) {
            const QChar c = clause.at(at);
            if (c == QLatin1Char('u'))
                who |= 0700;
            else if (c == QLatin1Char('g'))
                who |= 0070;
            else if (c == QLatin1Char('o'))
                who |= 0007;
            else if (c == QLatin1Char('a'))
                who |= 0777;
            else
                break;
        }
        if (who == 0 || at == clause.size())
            return invalid();
        while (at < clause.size()) {
            ModeStep step{who, clause.at(at).toLatin1(), 0};
            if (step.op != '+' && step.op != '-' && step.op != '=')
                return invalid();
            for (++at; at < clause.size(); ++at) {
                const QChar c = clause.at(at);
                if (c == QLatin1Char('r'))
                    step.perms |= 0444;
                else if (c == QLatin1Char('w'))
                    step.perms |= 0222;
                else if (c == QLatin1Char('x'))
                    step.perms |= 0111;
                else if (c == QLatin1Char('s') || c == QLatin1Char('t'))
                    return special();
                else
                    break;
            }
            steps->append(step);
        }
    }
    return true;
}

// The mode `steps` turn `old` into. The special bits pass through untouched.
mode_t appliedMode(mode_t old, const QVector<ModeStep> &steps)
{
    mode_t perms = old & 0777;
    for (const ModeStep &step : steps) {
        const mode_t bits = step.perms & step.who;
        if (step.op == '+')
            perms |= bits;
        else if (step.op == '-')
            perms &= ~bits;
        else
            perms = (perms & ~step.who) | bits;
    }
    return (old & 07000) | perms;
}

QString octalMode(mode_t mode)
{
    return QStringLiteral("%1").arg(mode & 07777, 4, 8, QLatin1Char('0'));
}

int cmdChmod(const Globals &g, const QStringList &positionals)
{
    QVector<ModeStep> steps;
    QString error;
    if (!parseMode(positionals.first(), &steps, &error)) {
        fail(error);
        return kUsage;
    }
    int status = kOk;
    const QStringList resolved = resolveNamed(QStringLiteral("chmod"), positionals.mid(1), &status);

    // A link is refused rather than followed: chmod(2) would change the file at
    // the far end, which is not the name that was handed in.
    const auto lstatOrSay = [&](const QString &path, struct stat *st) {
        if (::lstat(QFile::encodeName(path).constData(), st) == 0)
            return true;
        QString why;
        status = qMax(status, pathStatus(pathState(path, &why)));
        fail(why);
        return false;
    };
    for (const QString &path : resolved) {
        struct stat st {};
        if (!lstatOrSay(path, &st))
            continue;
        if (S_ISLNK(st.st_mode)) {
            fail(QStringLiteral("chmod: %1 is a symlink; chmod acts on names here and "
                                "refuses links, so name the file it leads to")
                     .arg(spelled(path)));
            status = qMax(status, kUsage);
        }
    }
    refuseKept(QStringLiteral("chmod"), resolved, &status, false);
    if (status != kOk)
        return status;

    QJsonArray done;
    bool refused = false;
    // What a dry run would have left on each inode, so two names of one file
    // preview the modes the run gives them.
    QMap<FileId, mode_t> planned;
    for (const QString &path : resolved) {
        // Read right before its own change, so the receipt's old mode is the
        // one this change replaced even where two operands are one inode.
        struct stat before {};
        if (!lstatOrSay(path, &before)) {
            refused = true;
            continue;
        }
        const FileId inode = fileId(before);
        const mode_t old = planned.value(inode, before.st_mode);
        mode_t now = appliedMode(old, steps);
        if (g.dryRun)
            planned.insert(inode, now);
        if (!g.dryRun) {
            const QByteArray name = QFile::encodeName(path);
            // NOFOLLOW so a link swapped in since the check above is refused by
            // the system rather than followed.
            if (::fchmodat(AT_FDCWD, name.constData(), now & 07777, AT_SYMLINK_NOFOLLOW) != 0) {
                fail(QStringLiteral("chmod: changing permissions of %1: %2")
                         .arg(spelled(path), systemReason(errno)));
                refused = true;
                continue;
            }
            // What the system kept, which can be less: it drops setgid for an
            // owner outside the file's group.
            struct stat after {};
            if (::lstat(name.constData(), &after) == 0)
                now = after.st_mode;
        }
        if (g.json)
            done.append(QJsonObject{{QStringLiteral("path"), path},
                                    {QStringLiteral("old"), octalMode(old)},
                                    {QStringLiteral("new"), octalMode(now)}});
        else
            out() << octalMode(old) << '\t' << octalMode(now) << '\t' << path << '\n';
    }
    if (g.json)
        printJson(QJsonObject{{QStringLiteral("modes"), done}});
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
    const QString dir = dirOrHere(QStringLiteral("reindex"), positionals, &status);
    if (status != kOk)
        return status;
    // A tree it cannot open is an operand refused, as `sweep` refuses one, so a
    // batch takes the run back.
    if (const QString shut = cannotLookInto(dir, true); !shut.isEmpty()) {
        fail(QStringLiteral("reindex: %1").arg(shutGap(dir, QStringLiteral("reindexed"), shut)));
        return kIncomplete;
    }

    if ((status = openSession(&s)) != kOk)
        return status;

    QVector<Record> written;
    // A path it walked away from is `s.incomplete`, which is where the exit
    // code below comes from.
    const Walk walk = walkTree(dir);
    // A tree it could not open, an entry it could not describe and a name no
    // row can hold are each part of the tree this rebuild did not settle.
    for (const Gap &gap : walkGaps(walk, QStringLiteral("reindexed"))) {
        fail(QStringLiteral("reindex: %1").arg(gap.said));
        s.incomplete = true;
    }
    QMap<QString, QString> unresolved;
    const QStringList targets = tagTargets(walk, &unresolved);
    for (const QString &why : std::as_const(unresolved)) {
        fail(QStringLiteral("reindex: %1; it was not reindexed").arg(why));
        s.incomplete = true;
    }
    // Hard links whose rows disagree: the file's one copy cannot mirror both,
    // so the index cannot win and neither row is written out.
    QSet<QString> torn;
    if (!fromXattr) {
        QMap<FileId, QStringList> names;
        for (const QString &path : targets) {
            if (const std::optional<FileId> id = fileIdOf(path); id && s.store.contains(path))
                names[*id].append(path);
        }
        for (const QStringList &same : std::as_const(names)) {
            const QStringList first = s.store.get(same.first()).tags;
            if (std::all_of(same.cbegin(), same.cend(), [&](const QString &path) {
                    return s.store.get(path).tags == first;
                }))
                continue;
            QStringList said;
            for (const QString &path : same)
                said.append(spelled(path));
            fail(QStringLiteral("reindex: %1 are one file whose rows hold different tags, "
                                "and its xattr can mirror only one; tag it by one name, or "
                                "`reindex --from-xattr` gives each name the file's own")
                     .arg(said.join(QStringLiteral(", "))));
            s.incomplete = true;
            torn.unite(QSet<QString>(same.cbegin(), same.cend()));
        }
    }
    for (const QString &path : targets) {
        if (torn.contains(path))
            continue;
        s.walked.insert(path);
        QStringList tags;
        QString warning;
        TagsValue held = TagsValue::None;
        // A file an earlier batch line settled, by any name, gets that row's
        // tags at the flush; the disk still holds the value being replaced.
        if (const std::optional<QStringList> copy = settledCopy(s, path)) {
            tags = *copy;
        } else if (!readTagsXattr(path, &tags, &warning, &held)) {
            fail(QStringLiteral("reindex: %1").arg(warning));
            s.incomplete = true;
            continue;
        }
        // A file that cannot hold a copy has only its row, so neither side
        // has anything to settle.
        if (held == TagsValue::Unsupported)
            continue;

        // The row is looked up before anything short-circuits, or the
        // commonest repair is out of reach: a file copied without
        // `--preserve=xattr` has a row and no xattr, which `verify` calls
        // `xattr-differs`.
        const Record existing = s.store.get(path);
        // What the file says, word for word as it says it, and what of that is
        // usable. The two are compared separately: `tags` is the value `verify`
        // calls `xattr-differs` over, order and repeats included, so rewriting
        // exactly when it differs is what makes the pair converge.
        const QStringList clean = salvagedTags(tags, nullptr);
        const bool unusable = !tags.isEmpty() && clean.isEmpty();

        const bool indexed = !existing.path.isEmpty();

        // The index keeps its answer, and then makes that true on disk --
        // whether the file's copy differs, is empty, or is something no tag
        // could be read out of.
        if (indexed && !fromXattr) {
            if (existing.tags != tags) {
                written.append(existing);
                mirrorTags(s, existing);
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
            if (unusable) {
                fail(QStringLiteral("reindex: %1: no tag can be read out of xattr %2%3; %4")
                         .arg(spelled(path), escapedWords(tags.join(QLatin1Char(','))),
                              indexed ? QStringLiteral(", so the index row is left alone")
                                      : QStringLiteral(" and there is no index row to fall "
                                                       "back on"),
                              clearHint(path, indexed)));
                s.incomplete = true;
            } else if (indexed) {
                // Declining is right -- an empty xattr almost always means the
                // filesystem dropped the value, not that somebody cleared the
                // tags -- but the message has to name the command that does
                // converge, the way the two above do.
                fail(QStringLiteral("reindex: %1 has no tags of its own; the index "
                                    "row is left alone -- plain `reindex` writes it "
                                    "back over the file's copy").arg(spelled(path)));
                s.incomplete = true;
            }
            continue;
        }

        QString error;
        const Record record = stamped(path, clean, &error, nullptr, existing);
        if (record.path.isEmpty()) {
            // Walked past between the walk and here, or the system stopped
            // talking about it. Either way this run did not settle it.
            fail(error);
            s.incomplete = true;
            continue;
        }
        // The row and the file both have to end up saying it. Comparing only
        // the row would let a file keep a value the row does not hold -- row
        // `good`, xattr `Good,good` -- which `verify` goes on calling
        // `xattr-differs`.
        if (existing.tags == record.tags && tags == record.tags)
            continue;
        s.store.put(record);
        written.append(record);
        // The file's own copy follows the row it mirrors, the way it does
        // everywhere else -- but only where the two do not already agree, the
        // same skip the plain path above takes. Writing a value straight back
        // onto the file it was just read from is a ctime bump for nothing on a
        // writable file, and on one the filesystem will not take a write for it
        // is exit 1 from a run that settled every path it walked.
        if (tags != record.tags)
            mirrorTags(s, record);
    }

    const int committed = finish(g, s, written);
    if (committed != kOk)
        return committed;
    // Says what it touched, for a verb whose whole job is to change rows -- but
    // not in a dry run, where `settle` is already printing the same list.
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
        for (const Record &record : written) {
            if (changedBy(s, record.path))
                out() << record.path << '\n';
        }
    }
    // A run that walked away from a path did not do all of what it was asked,
    // and that is a nonzero exit everywhere else on the system -- `cp`, `chmod`
    // and `rm -f` all report one file they could not handle that way. A file
    // whose copy the filesystem would not take is one of those paths, and the
    // flush inside `finish` is where it was counted.
    return settledStatus(s);
}

// ------------------------------------------------------------------- sweep

// How long `sweep` waits for another writer before it gives up.
//
// Every other verb waits five seconds and then exits 3, which is right for a
// command somebody typed and watched. This is the one meant to run when nobody
// asked it to, so it does the opposite: not at all, and a lock it does not get is
// not a failure -- it is a run that stepped aside, and the next one does the
// same work over the same trees. Anything else would mean that tagging a file
// in the window while a sweep happened to be walking either stalled the window
// or mailed somebody an exit 3, and a routine that does either is a routine
// that gets turned off.
constexpr int kSweepLockWait = 0;

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
    QStringList notHere;      // declared roots not swept, away or not to be checked
    int listed = 0;           // paths written to a root's list of names
    int unlistable = 0;       // names the list's one-path-to-a-line format cannot hold
    /// This tree's paths whose one change is the file's own copy, which count as
    /// merged once the flush has written it.
    QStringList mirroring;
};

QJsonArray asJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

// A row the run looks at and does not sweep: gone, or skipped where it is on a
// disk in a drawer or nobody may look -- "I may not look" is not "it is gone",
// the distinction `prune` turns on, and this verb counts rather than acts on.
void countRow(const QString &path, SweepRun *run)
{
    const PathState state = rowState(path, nullptr);
    if (state == PathState::Missing)
        run->gone.append(path);
    else if (state != PathState::Present)
        ++run->skipped;
}

// One tree, under one hold of the lock.
//
// The hold is the whole reason this is a pass and not a walk: the window writes
// through this same CLI now, so a sweep that held the index across every
// declared root would surface as `store: cannot lock` in somebody's row menu.
// One tree, one lock, one commit, and the lock goes back between them.
//
// A tree of `trees` nested inside this one is its own pass, so this one leaves
// it apart: each path is swept once.
void sweepOneTree(const Globals &g, Session &s, const QString &tree, const QStringList &trees,
                  SweepRun *run, QVector<Record> *written, const Root *cacheFor)
{
    QSet<QString> apart;
    for (const QString &other : trees) {
        if (other != tree && isUnderDirectory(other, tree))
            apart.insert(other);
    }
    const auto holds = [&apart](const QString &path) {
        return std::any_of(apart.cbegin(), apart.cend(), [&path](const QString &nested) {
            return isUnderDirectory(path, nested);
        });
    };
    // The walk is the expensive half of this verb and two things want it: the
    // tags each file carries, and the plain list of what is there. Collecting
    // the second costs a string per path and saves a second walk of the disk --
    // see `Names.h` for what that list is and is not.
    const Walk walk = walkTree(tree, apart);
    // Asked before any row is written, so a row this run is about to write is
    // not then looked up as a row whose file might be gone. An entry the walk
    // could not describe is counted once, as unreadable, below.
    for (const Record &record : s.store.records()) {
        if (isUnderDirectory(record.path, tree) && !holds(record.path)
            && !walk.undescribed.contains(record.path))
            countRow(record.path, run);
    }
    // What the walk could not reach is a finding like a file nobody may read:
    // counted and said, and the run goes on.
    run->unreadable += walk.shut;
    run->unreadable += walk.undescribed.keys();
    for (const Gap &gap : walkGaps(walk, QStringLiteral("swept")))
        warn(g, QStringLiteral("sweep: %1").arg(gap.said));
    QMap<QString, QString> unresolved;
    const QStringList targets = tagTargets(walk, &unresolved);
    for (auto link = unresolved.cbegin(); link != unresolved.cend(); ++link) {
        run->unreadable.append(link.key());
        warn(g, QStringLiteral("sweep: %1; it was not swept").arg(link.value()));
    }
    for (const QString &path : targets) {
        QStringList carried;
        QString warning;
        if (!readTagsXattr(path, &carried, &warning)) {
            run->unreadable.append(path);
            warn(g, QStringLiteral("sweep: %1").arg(warning));
            continue;
        }
        const QStringList clean = salvagedTags(carried, nullptr);
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
            // The tags changed, so the stamp is now's.
            const Record record = stamped(path, united, &error, nullptr);
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
        // -- the other half of the union -- where it lacks any. See
        // `flushPending` for a value it will not rewrite.
        if (clean != united) {
            Record mirror;
            mirror.path = path;
            mirror.tags = united;
            mirrorTags(s, mirror);
            if (!changed)
                run->mirroring.append(path);
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
    if (!cacheFor)
        return;
    // The same entries plain `ls --untagged` lists, links by their own names,
    // and the same gaps it would name, so `--cached` can say it is short. A
    // root declared inside this one keeps its own list, so its paths stay out.
    const auto owned = [cacheFor](const QString &path) { return listedUnder(path, *cacheFor); };
    QStringList names;
    for (const QString &path : walk.paths) {
        if (owned(path))
            names.append(relativeTo(path, walk.base));
    }
    // The root's own directory is its gap and nobody else's: shut, it is the
    // whole list.
    const auto gapOwned = [cacheFor, &owned](const QString &path) {
        return path == cacheFor->path || owned(path);
    };
    // A dry run counts the list it would write.
    int dropped = 0;
    QString error;
    if (g.dryRun) {
        dropped = int(std::count_if(names.cbegin(), names.cend(),
                                    [](const QString &name) { return !listable(name); }));
    } else if (!writeNames(cacheFor->id, names,
                           walkGaps(walk, QStringLiteral("listed"), gapOwned), &dropped, &error)) {
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
// for a directory rather than handed the current one. A root here declared
// inside the named tree is a pass of its own, so its list of names is built;
// one that is not here is the walk's to report.
QStringList sweepTrees(const Globals &g, const QStringList &positionals, SweepRun *run,
                       int *status)
{
    QString dir = optionalDir(QStringLiteral("sweep"), positionals, status);
    if (*status != kOk)
        return {};
    if (!dir.isEmpty()) {
        if (const QString shut = cannotLookInto(dir, true); !shut.isEmpty()) {
            fail(QStringLiteral("sweep: %1").arg(shutGap(dir, QStringLiteral("swept"), shut)));
            *status = kIncomplete;
            return {};
        }
        QStringList trees{dir};
        for (const Root &root : roots()) {
            if (root.path != dir && isUnderDirectory(root.path, dir)
                && rootState(root, nullptr) == PathState::Present)
                trees.append(root.path);
        }
        return trees;
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
        // A root that is not here is not a failure: that is the disk in the
        // drawer this whole distinction exists for. One nobody may check is
        // counted and said, and the rest are still swept.
        QString why;
        const PathState storage = rootState(root, &why);
        if (storage == PathState::Unreachable) {
            warn(g, QStringLiteral("sweep: root %1 is declared and not here; it was not swept")
                        .arg(root.id));
            run->notHere.append(root.path);
            continue;
        }
        if (storage != PathState::Present) {
            warn(g, QStringLiteral("sweep: root %1 cannot be checked: %2; it was not swept")
                        .arg(root.id, why));
            run->unreadable.append(root.path);
            run->notHere.append(root.path);
            continue;
        }
        trees.append(root.path);
    }
    return trees;
}

// The counts a receipt keeps, in the order it prints them.
const char *const kSweepCounts[] = {"adopted",  "merged", "gone",      "unreadable",
                                    "skipped",  "listed", "unlistable"};

// A run's answer under `--json`: every key, whatever it found.
QJsonObject sweepJson(const SweepRun &run)
{
    return QJsonObject{{QStringLiteral("trees"), asJsonArray(run.trees)},
                       {QStringLiteral("adopted"), asJsonArray(run.adopted)},
                       {QStringLiteral("merged"), asJsonArray(run.merged)},
                       {QStringLiteral("gone"), asJsonArray(run.gone)},
                       {QStringLiteral("unreadable"), asJsonArray(run.unreadable)},
                       {QStringLiteral("skipped"), run.skipped},
                       {QStringLiteral("listed"), run.listed},
                       {QStringLiteral("unlistable"), run.unlistable}};
}

// A run's counts, one number per `kSweepCounts` field.
QJsonObject sweepCounts(const SweepRun &run)
{
    QJsonObject counts;
    const QJsonObject found = sweepJson(run);
    for (const char *field : kSweepCounts) {
        const QString key = QString::fromLatin1(field);
        const QJsonValue value = found.value(key);
        counts.insert(key, value.isArray() ? value.toArray().size() : value.toInt());
    }
    return counts;
}

// A whole number, not negative, that a `qint64` holds.
bool isCount(const QJsonValue &value)
{
    // The integer the JSON holds, where it holds one: a double rounds 2^63-1 up.
    return value.toInteger(-1) >= 0;
}

// Whether `receipt` has the shape `writeSweepReceipt` writes: a stamp that is a
// count and a moment a clock can print, a duration that is a number, trees that
// are absolute paths in their clean form, and counts.
bool isSweepReceipt(const QJsonObject &receipt)
{
    const QJsonValue at = receipt.value(QStringLiteral("at"));
    const QJsonValue seconds = receipt.value(QStringLiteral("seconds"));
    if (!isCount(at) || !QDateTime::fromSecsSinceEpoch(at.toInteger()).isValid()
        || !seconds.isDouble() || seconds.toDouble() < 0
        || !receipt.value(QStringLiteral("trees")).isArray())
        return false;
    for (const QJsonValue &tree : receipt.value(QStringLiteral("trees")).toArray()) {
        if (!isCleanAbsolute(tree.toString()))
            return false;
    }
    for (const char *field : kSweepCounts) {
        if (!isCount(receipt.value(QString::fromLatin1(field))))
            return false;
    }
    return true;
}

int printSweepStatus(const Globals &g)
{
    const QString path = sweepReceiptPath();
    QFile file(path);
    QString why;
    const PathState opened = openToRead(&file, {}, &why);
    if (opened != PathState::Present && opened != PathState::Missing) {
        fail(QStringLiteral("sweep: cannot read %1").arg(why));
        return kIncomplete;
    }
    // A receipt nobody has written yet is an answer and not an error: it says
    // the routine has never run, which is what somebody asking is asking.
    const bool never = opened == PathState::Missing;
    QJsonObject receipt;
    if (!never && (!keptObject(file.readAll(), &receipt, &why) || !isSweepReceipt(receipt))) {
        fail(QStringLiteral("sweep: %1 is not a receipt; the next sweep writes a "
                            "new one").arg(spelled(path)));
        return kIncomplete;
    }
    // The documented keys and nothing else the file holds.
    if (g.json) {
        QJsonObject shown{
            {QStringLiteral("at"),
             never ? QJsonValue() : QJsonValue(receipt.value(QStringLiteral("at")).toInteger())},
            {QStringLiteral("seconds"),
             never ? QJsonValue() : receipt.value(QStringLiteral("seconds"))},
            {QStringLiteral("trees"), receipt.value(QStringLiteral("trees")).toArray()}};
        for (const char *field : kSweepCounts) {
            const QString key = QString::fromLatin1(field);
            shown.insert(key, receipt.value(key).toInteger());
        }
        printJson(shown);
        return kOk;
    }
    if (never) {
        out() << "never\n";
        return kOk;
    }
    // The shape `tags` and `roots ls` print in: one fact to a line, tab
    // separated, so `cut -f2` gets the value out of any of them.
    const qint64 at = receipt.value(QStringLiteral("at")).toInteger();
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
    for (const char *field : kSweepCounts) {
        out() << field << '\t' << receipt.value(QString::fromLatin1(field)).toInteger() << '\n';
    }
    return kOk;
}

// A receipt it could not write is a note and not the run: the sweep landed.
void writeSweepReceipt(const Globals &g, const SweepRun &run, qint64 milliseconds)
{
    const QString path = sweepReceiptPath();
    QJsonObject receipt = sweepCounts(run);
    receipt.insert(QStringLiteral("at"), QDateTime::currentSecsSinceEpoch());
    receipt.insert(QStringLiteral("seconds"), milliseconds / 1000.0);
    receipt.insert(QStringLiteral("trees"), asJsonArray(run.trees));
    QString why;
    if (!writeKeptLines(path, {receipt}, &why)) {
        warn(g, QStringLiteral("sweep: cannot write the receipt %1: %2")
                    .arg(spelled(path), why));
    }
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
    if (status)
        return printSweepStatus(g);

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    SweepRun run;
    int scopeStatus = kOk;
    const QStringList trees = sweepTrees(g, positionals, &run, &scopeStatus);
    if (scopeStatus != kOk)
        return scopeStatus;
    // Roots are declared and not one of them is here, or none could be checked,
    // which was said above. Every disk in a drawer at once is unusual and not
    // wrong, and it is still a run: its counts are said and its receipt kept.
    if (trees.isEmpty()) {
        warn(g, run.unreadable.isEmpty()
                    ? QStringLiteral("sweep: no declared root is here; nothing was swept")
                    : QStringLiteral("sweep: nothing was swept"));
    }

    QStringList untouched;
    for (int i = 0; i < trees.size(); ++i) {
        const QString &tree = trees.at(i);
        QString error;
        if (g.dryRun) {
            if (!s.opened && !s.store.load(&error)) {
                fail(error);
                return kStore;
            }
        } else {
            s.store.setLockTimeout(kSweepLockWait);
            // Only another writer is stepped aside for. An index that will not
            // read, or a lock the system refuses, is the exit 3 of any writer.
            if (!s.store.lockAndLoad(&error)) {
                if (!s.store.contended()) {
                    fail(error);
                    return kStore;
                }
                untouched = trees.mid(i);
                break;
            }
        }
        s.opened = true;

        QVector<Record> written;
        // A list of names belongs to a root, so it is written only where the
        // tree being swept is one. `sweep` with no `[dir]` is every declared
        // root, which is how the caches get built without anybody asking.
        sweepOneTree(g, s, tree, trees, &run, &written, rootAt(tree));
        run.trees.append(tree);
        // `announce` is false because the run has one answer for every tree it
        // swept and prints it below; a per-tree array would be one JSON
        // document per pass on a stdout that carries one.
        const int committed = finish(g, s, written, false);
        if (committed != kOk)
            return committed;
        // A copy the flush did not write changed nothing, and the flush said so.
        for (const QString &path : std::as_const(run.mirroring)) {
            if (s.mirrored.contains(path))
                run.merged.append(path);
        }
        run.mirroring.clear();
    }

    // The rows under a root no pass walked, counted once each: one inside a
    // swept tree was counted by its pass, and one inside a tree it stepped
    // aside from was not.
    if (!run.notHere.isEmpty()) {
        QString error;
        if (!s.opened && !s.store.load(&error)) {
            fail(error);
            return kStore;
        }
        const auto under = [](const QString &path, const QStringList &trees) {
            return std::any_of(trees.cbegin(), trees.cend(), [&path](const QString &tree) {
                return isUnderDirectory(path, tree);
            });
        };
        for (const Record &record : s.store.records()) {
            if (under(record.path, run.notHere) && !under(record.path, run.trees))
                countRow(record.path, &run);
        }
    }

    if (!untouched.isEmpty()) {
        QStringList trees;
        for (const QString &tree : std::as_const(untouched))
            trees.append(spelled(tree));
        warn(g, QStringLiteral("sweep: another writer holds the index; stepped aside "
                               "with %1 tree(s) unswept -- %2")
                    .arg(untouched.size())
                    .arg(trees.join(QStringLiteral(", "))));
    }
    // A root read in the old case of a name another program re-cased is written
    // as its directory stores it, as its rows were.
    if (QString error; !g.dryRun && rootsRespelled()
                       && !saveRoots(defaultRootsPath(), roots(), &error)) {
        fail(error);
        return kStore;
    }
    // Every run but one that stepped aside before it swept anything.
    if (!g.dryRun && (untouched.isEmpty() || !run.trees.isEmpty()))
        writeSweepReceipt(g, run, QDateTime::currentMSecsSinceEpoch() - started);

    if (g.json) {
        printJson(sweepJson(run));
    } else {
        QStringList touched = run.adopted + run.merged;
        touched.sort();
        for (const QString &path : touched)
            out() << path << '\n';
    }

    // The summary is a note beside a run that succeeded, which is what stderr
    // is for here and what `--quiet` is documented to take away. The counts are
    // the whole point of the verb -- what it changed, and what it found and
    // deliberately did not.
    // Every count, whatever it found, the way `--json` has every key.
    QStringList parts;
    const QJsonObject counts = sweepCounts(run);
    for (const char *field : kSweepCounts) {
        parts << QStringLiteral("%1 %2").arg(QLatin1String(field))
                     .arg(counts.value(QString::fromLatin1(field)).toInt());
    }
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

    if (sub == QLatin1String("ls")) {
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
            // quoted to paste, so a view is a thing you can read as well as open.
            for (const View &view : std::as_const(views)) {
                QString asks = QStringLiteral("omafiles ls");
                for (const QString &tag : view.tags)
                    asks += QStringLiteral(" --tag ") + shellQuoted(tag);
                if (!view.dir.isEmpty())
                    asks += QLatin1Char(' ') + shellQuoted(view.dir);
                out() << view.name << '\t' << asks << '\n';
            }
        }
        return kOk;
    }

    if (sub == QLatin1String("add")) {
        const QString clean = normalizeViewName(name, &error);
        if (clean.isEmpty()) {
            fail(QStringLiteral("view add: %1").arg(error));
            return kUsage;
        }
        if (tags.isEmpty() && positionals.isEmpty()) {
            fail(QStringLiteral("view add: a view asks something; give it a --tag or a folder"));
            return kUsage;
        }

        View view;
        view.name = clean;
        view.tags = tags;
        if (!positionals.isEmpty()) {
            if (closedStdin(QStringLiteral("view add"), positionals.first()))
                return kIncomplete;
            PathState state = PathState::Missing;
            view.dir = canonicalPath(positionals.first(), &error, &state);
            if (view.dir.isEmpty()) {
                fail(QStringLiteral("view add: %1").arg(error));
                return pathStatus(state);
            }
            if (!QFileInfo(view.dir).isDir()) {
                fail(QStringLiteral("view add: %1 is not a folder").arg(spelled(view.dir)));
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

        if (!g.dryRun && !saveViews(file, views, &error)) {
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
        // Read the way `view add` reads it; a name it would refuse names no view.
        const QString wanted = normalizeViewName(name, nullptr);
        QVector<View> kept;
        QString removed;
        for (const View &view : std::as_const(views)) {
            if (!wanted.isEmpty() && view.name.compare(wanted, Qt::CaseInsensitive) == 0)
                removed = view.name;             // as it was saved, the receipt `view add` gives
            else
                kept.append(view);
        }
        if (removed.isEmpty()) {
            fail(QStringLiteral("view rm: no view called %1").arg(spelled(name)));
            return kUsage;
        }
        if (!g.dryRun && !saveViews(file, kept, &error)) {
            fail(error);
            return kStore;
        }
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("name"), removed}});
        else if (g.dryRun)
            out() << removed << '\n';
        return kOk;
    }

    Q_UNREACHABLE_RETURN(kUsage);
}

// The declarations, and the two ways to change them.
//
// Read as well as written here rather than left to a text editor, because the
// interesting half of the answer is not in the file: whether the storage is
// actually here right now is a question about this machine at this moment, and
// it is the one that decides whether `prune` may touch the rows under it.
int cmdRoots(const Globals &g, Session &s, const QString &sub, const QString &id,
             bool mount, const std::optional<QString> &check, const QStringList &positionals)
{
    const QString file = defaultRootsPath();
    QVector<Root> declared = roots();

    if (sub == QLatin1String("ls")) {
        // `unknown` where the system would not say, which is neither answer.
        QStringList unchecked;
        QJsonArray array;
        for (const Root &root : declared) {
            QString why;
            const PathState storage = rootState(root, &why);
            if (storage != PathState::Present && storage != PathState::Unreachable)
                unchecked.append(QStringLiteral("roots ls: %1 cannot be checked: %2")
                                     .arg(root.id, why));
            const QJsonValue here = storage == PathState::Present ? QJsonValue(true)
                : storage == PathState::Unreachable              ? QJsonValue(false)
                                                                 : QJsonValue();
            if (g.json) {
                array.append(QJsonObject{
                    {QStringLiteral("id"), root.id},
                    {QStringLiteral("path"), root.path},
                    {QStringLiteral("mount"), root.mount},
                    {QStringLiteral("check"), root.check},
                    {QStringLiteral("here"), here},
                });
            } else {
                out() << root.id << '\t' << root.path << '\t'
                      << (here.isNull() ? "unknown" : here.toBool() ? "here" : "away") << '\n';
            }
        }
        if (g.json)
            printJson(array);
        for (const QString &note : std::as_const(unchecked))
            fail(note);
        return unchecked.isEmpty() ? kOk : kIncomplete;
    }

    if (sub == QLatin1String("add")) {
        QString error;
        // The id is a name and not a path: every index row names its root, and
        // the name cache is one file per root with this for a file name.
        const QString wanted = normalizeRootId(id, &error);
        if (wanted.isEmpty()) {
            fail(QStringLiteral("roots add: %1").arg(error));
            return kUsage;
        }
        // Resolved, because everything compared against it is: a root declared
        // through a symlink would hold no rows at all.
        if (closedStdin(QStringLiteral("roots add"), positionals.first()))
            return kIncomplete;
        PathState state = PathState::Missing;
        QString path = canonicalPath(positionals.first(), &error, &state);
        // A mount point udisks makes on mounting and removes on unmounting is
        // declared unplugged as `realpath -m` names it: the part that is here
        // resolved, and plain names after it.
        if (path.isEmpty() && mount && state == PathState::Missing
            && danglingChain(positionals.first()).isEmpty()) {
            const QString named = storedSpelling(physicalPath(positionals.first()));
            if (QDir::cleanPath(named) == named) {
                path = named;
                state = PathState::Unreachable;
            }
        }
        if (path.isEmpty()) {
            fail(QStringLiteral("roots add: %1").arg(error));
            return pathStatus(state);
        }
        if (state == PathState::Present && !QFileInfo(path).isDir()) {
            fail(QStringLiteral("roots add: %1 is not a directory").arg(spelled(path)));
            return kUsage;
        }
        for (const Root &root : declared) {
            if (root.id == wanted) {
                fail(QStringLiteral("roots add: %1 is already declared, at %2")
                         .arg(wanted, spelled(root.path)));
                return kUsage;
            }
            if (root.path == path) {
                fail(QStringLiteral("roots add: %1 is already declared, as %2")
                         .arg(spelled(path), root.id));
                return kUsage;
            }
        }
        // The marker is named inside the root, so the declaration survives a
        // remount elsewhere, and it is asked for now, while the storage is
        // here: one missing today is a typo that would read `away` forever.
        const Root root{wanted, path, mount, check.value_or(QString())};
        if (check) {
            if (normalizeRootCheck(*check, &error).isEmpty()) {
                fail(QStringLiteral("roots add: %1").arg(error));
                return kUsage;
            }
            QString why;
            const PathState marker = rootState(root, &why);
            if (marker != PathState::Present) {
                fail(marker == PathState::Unreachable
                         ? QStringLiteral("roots add: --check %1 is not there under %2; name "
                                          "a path the storage holds now")
                               .arg(spelled(root.check), spelled(path))
                         : QStringLiteral("roots add: --check %1 cannot be checked: %2")
                               .arg(spelled(root.check), why));
                return kUsage;
            }
            // Resolved by the kernel: a check that leads out of the root, or
            // back to it, answers for other storage and reads `here` always.
            const QString reached = realPath(QDir(path).filePath(root.check), &why);
            if (reached.isEmpty() || reached == path || !isUnderDirectory(reached, path)) {
                fail(reached.isEmpty()
                         ? QStringLiteral("roots add: --check %1 cannot be checked: %2")
                               .arg(spelled(root.check), why)
                         : QStringLiteral("roots add: --check %1 leads to %2, which is not "
                                          "inside %3; name a path the storage holds")
                               .arg(spelled(root.check), spelled(reached), spelled(path)));
                return kUsage;
            }
        }
        declared.append(root);
        if (!g.dryRun) {
            // A list a root of this id left behind is not this root's.
            if (!forgetNames(wanted, &error)) {
                fail(QStringLiteral("roots add: %1").arg(error));
                return kStore;
            }
            if (!saveRoots(file, declared, &error)) {
                fail(error);
                return kStore;
            }
            setRoots(declared);
        }
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("id"), wanted},
                                  {QStringLiteral("path"), path}});
        else
            out() << path << '\n';
        return kOk;
    }

    if (sub == QLatin1String("rm")) {
        // Folded the way `add` folded it, so `--id Archive` reaches the root
        // it declared.
        const QString wanted = normalizeRootId(id, nullptr);
        if (wanted.isEmpty() || !rootById(wanted)) {
            fail(QStringLiteral("roots rm: %1 is not declared").arg(spelled(id)));
            return kUsage;
        }
        const QString removedAt = rootById(wanted)->path;
        // The rows under it come with it. A row names a declaration, so leaving them behind
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

        // Each row is rewritten in the form the roots left give it: an
        // absolute path, or relative to a root declared inside this one.
        int carried = 0;
        QMap<QString, int> landed;
        for (const Record &record : s.store.records()) {
            if (record.root != wanted)
                continue;
            Record rewritten = record;
            splitByRoot(record.path, &rewritten.root, &rewritten.relative);
            s.store.put(rewritten);
            ++carried;
            ++landed[rewritten.root];
        }
        if (carried > 0)
            s.store.rewrite();
        const auto said = [&](const QString &verbed) {
            if (landed.isEmpty())
                out() << "0 row(s) " << verbed << '\n';
            for (auto where = landed.cbegin(); where != landed.cend(); ++where) {
                out() << where.value() << " row(s) " << verbed << ' '
                      << (where.key().isEmpty()
                              ? QStringLiteral("as absolute paths")
                              : QStringLiteral("under root %1").arg(where.key()))
                      << '\n';
            }
        };

        QString error;
        if (!g.dryRun && !s.store.commit(&error)) {
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
                said(QStringLiteral("would be written back"));
            return kOk;
        }
        if (!saveRoots(file, kept, &error)) {
            fail(error);
            return kStore;
        }
        // The list of names goes with the declaration: it is one file per root,
        // and a root nobody declares is one nothing will ever read a list for.
        // Losing it costs a sweep and no information, which is what a cache is.
        // A root around it left its paths out of its own list, which is short
        // now: dropped too, so `--cached` asks for the sweep that rebuilds it.
        QStringList stuck;
        if (!forgetNames(wanted, &error))
            stuck.append(error);
        for (const Root &root : std::as_const(kept)) {
            if (isUnderDirectory(removedAt, root.path) && !forgetNames(root.id, &error))
                stuck.append(error);
        }
        if (g.json)
            printJson(QJsonObject{{QStringLiteral("id"), wanted},
                                  {QStringLiteral("rows"), carried}});
        else
            said(QStringLiteral("written back"));
        for (const QString &reason : std::as_const(stuck))
            fail(QStringLiteral("roots rm: %1").arg(reason));
        return stuck.isEmpty() ? kOk : kStore;
    }

    if (sub == QLatin1String("migrate")) {
        // A read and a write of the same rows, which is all a migration is: the
        // form on disk is decided when a row is written, so every row is
        // rewritten under whichever root holds it now.
        int status = kOk;
        if ((status = openSession(&s)) != kOk)
            return status;
        int rooted = 0;
        int absolute = 0;
        for (const Record &record : s.store.records()) {
            if (rootFor(record.path))
                ++rooted;
            else
                ++absolute;
        }
        s.store.rewrite();
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

    Q_UNREACHABLE_RETURN(kUsage);
}

// Exit 1 when it found something, the way `diff` does: a check that always
// succeeds is a check nothing can be built on.
int cmdVerify(const Globals &g, Session &s, const QStringList &positionals)
{
    // One answer to "where": `verify` and `verify .` are the same question, so
    // both sides take the same root rather than the disk walk falling back to
    // the current directory while the index side checks every row.
    int status = kOk;
    const QString root = dirOrHere(QStringLiteral("verify"), positionals, &status);
    if (status != kOk)
        return status;

    if ((status = openSession(&s)) != kOk)
        return status;

    QJsonArray problems;
    int found = 0;
    QSet<QString> seen;
    const auto report = [&](const char *kind, const QString &path, const QString &detail) {
        ++found;
        if (g.json) {
            problems.append(QJsonObject{{QStringLiteral("problem"),
                                         QString::fromLatin1(kind)},
                                        {QStringLiteral("path"), path},
                                        {QStringLiteral("detail"), escapedWords(detail, true)}});
        } else {
            out() << QString::fromLatin1(kind) << '\t' << path;
            if (!detail.isEmpty())
                out() << '\t' << escapedWords(detail);
            out() << '\n';
        }
    };
    // A value holding a word that is not a tag is one only `tag clear` may
    // rewrite, with a row beside it or without, so it has a kind of its own
    // that names that command rather than one naming a rebuild. True where
    // `words` are such a value, reported.
    const auto invalid = [&](const QString &path, const QStringList &words, bool indexed) {
        QStringList rejected;
        const bool none = salvagedTags(words, &rejected).isEmpty();
        if (rejected.isEmpty())
            return false;
        report("xattr-invalid", path,
               QStringLiteral("xattr=%1; %2 -- %3")
                   .arg(words.join(QLatin1Char(',')),
                        none ? QStringLiteral("no tag can be read out of it")
                             : QStringLiteral("it holds words that are not tags"),
                        clearHint(path, indexed)));
        return true;
    };

    // The other direction: walking only the index would be blind to every file
    // carrying tags the index never heard of, which is what a lost write, or a
    // copy that brought its attributes along, leaves behind.
    //
    // Taken first because the index side below needs its answer: a directory
    // nobody may open fails its xattr read for that one reason, and reporting
    // the consequence there and the cause here is two problems where there is
    // one.
    QStringList unreachable;
    const Walk walk = walkTree(root);
    const QSet<QString> closedDirs(walk.shut.cbegin(), walk.shut.cend());

    for (const Record &record : s.store.records()) {
        if (!isUnderDirectory(record.path, root))
            continue;
        QString error;
        const PathState state = rowState(record.path, &error);
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
        TagsValue held = TagsValue::None;
        if (!readTagsXattr(record.path, &tags, &warning, &held)) {
            report("xattr-unreadable", record.path, warning);
            continue;
        }
        // A file that cannot hold a copy has only its row, which is no problem.
        if (held == TagsValue::Unsupported)
            continue;
        if (invalid(record.path, tags, true))
            continue;
        // Word for word: a value in another order or with a repeat is a
        // spelling this program never writes, and `reindex` rewrites it.
        if (tags != record.tags) {
            report("xattr-differs", record.path,
                   QStringLiteral("index=%1 xattr=%2")
                       .arg(record.tags.join(QLatin1Char(',')), tags.join(QLatin1Char(','))));
        }
    }

    for (const QString &closed : walk.shut) {
        // What the walk did, not what nobody else could: rows the index already
        // holds under it are reported on their own lines, each unreadable for
        // this same reason, so a detail claiming nothing under it was looked at
        // would be false on the very lines beside it.
        report("unreadable", closed,
               QStringLiteral("the tree under it was not walked: %1")
                   .arg(walk.shutWhy.value(closed)));
        // Its xattr cannot be read either, for the same reason. Saying so twice
        // is two lines about one cause.
        seen.insert(closed);
    }
    for (const QString &mangled : walk.unnamed) {
        // The same kind: a path this run cannot address, so it cannot say
        // anything about the path or the tree under it. The detail is what
        // separates the two causes, because the repairs differ -- a chmod or a
        // mount there, a rename here.
        report("unreadable", mangled,
               QStringLiteral("the name is not valid UTF-8; neither it nor the tree "
                              "under it was walked"));
    }
    for (auto refused = walk.undescribed.cbegin(); refused != walk.undescribed.cend();
         ++refused) {
        if (seen.contains(refused.key()))
            continue;
        seen.insert(refused.key());
        report("unreadable", refused.key(), refused.value());
    }
    QMap<QString, QString> unresolved;
    const QStringList targets = tagTargets(walk, &unresolved);
    for (auto link = unresolved.cbegin(); link != unresolved.cend(); ++link)
        report("unreadable", link.key(), reasonOnly(link.value(), link.key()));
    for (const QString &canonical : targets) {
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
        if (invalid(canonical, tags, false))
            continue;
        report("unindexed", canonical,
               QStringLiteral("xattr=%1").arg(tags.join(QLatin1Char(','))));
    }

    // Said after the problems, because it is not one of them: a row under a
    // root that is not here was not checked, and that is a fact about the
    // machine rather than about the index. A tree under one was not walked, for
    // the same reason.
    unreachable += walk.away.values();
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
    FileShape shape;
    QString since;
    QString from;
    QString id;
    QString check;
    QString name;
    QString replace;
    QString with;
    QString pattern;
    // Every flag typed, by its long name, whatever it carried: an empty value,
    // `--since ""` from an unset `$WINDOW`, reaches validation.
    QSet<QString> given;
    bool typed(const char *flag) const { return given.contains(QLatin1String(flag)); }
    bool desktop = false;
    bool mount = false;
    bool untagged = false;
    bool cached = false;
    bool fromXattr = false;
    bool all = false;
    bool status = false;
    QStringList positionals;
};

// The flags and operands `verb` (and its subcommand `sub`) takes, as
// `takesFor` says, and nothing else. A lone `-` is an operand, POSIX Utility
// Syntax Guideline 13's standard input.
int parseFlags(QStringList args, Parsed *parsed, const QString &verb, const QString &sub = {})
{
    const Takes takes = takesFor(verb, sub);
    const QString named = sub.isEmpty() ? verb : verb + QLatin1Char(' ') + sub;
    const auto refuse = [&named](const QString &why) {
        fail(QStringLiteral("%1: %2").arg(named, why));
        return kUsage;
    };
    QSet<QString> &given = parsed->given;
    for (int i = 0; i < args.size(); ++i) {
        const QString &arg = args.at(i);
        if (arg == QLatin1String("--")) {
            parsed->positionals.append(args.mid(i + 1));
            break;
        }
        if (!arg.startsWith(QLatin1Char('-')) || arg == QLatin1String("-")) {
            parsed->positionals.append(arg);
            continue;
        }
        const Flag *flag = flagNamed(arg);
        if (const QString refused = valueRefused(arg); !flag && !refused.isEmpty())
            return refuse(refused);
        if (!flag)
            return refuse(QStringLiteral("unknown flag: %1").arg(spelled(arg)));
        if (!(takes.*flag->gate))
            return refuse(QStringLiteral("unexpected %1").arg(arg));
        const QString name = flag->name;
        QString value;
        if (flag->value != Value::None) {
            // A flag that holds one value would have the second silently
            // replace the first.
            if (given.contains(name) && !repeats(*flag, takes))
                return refuse(QStringLiteral("%1 was given twice; it takes one value").arg(arg));
            if (i + 1 >= args.size())
                return refuse(QStringLiteral("%1 needs a value").arg(arg));
            value = args.at(++i);
        }
        given.insert(name);
        if (name == QLatin1String("--tag")) {
            parsed->tags.append(value);
        } else if (name == QLatin1String("--any")) {
            parsed->anys.append(value);
        } else if (name == QLatin1String("--untagged")) {
            parsed->untagged = true;
        } else if (name == QLatin1String("--cached")) {
            parsed->cached = true;
        } else if (name == QLatin1String("--ext")) {
            QString error;
            const QString ext = normalizeExt(value, &error);
            if (ext.isEmpty())
                return refuse(error);
            parsed->exts.append(ext);
        } else if (name == QLatin1String("--type")) {
            // fd's spellings, and find's `-type f` and `-type d`.
            if (value == QLatin1String("f") || value == QLatin1String("file"))
                parsed->shape.kind = QStringLiteral("file");
            else if (value == QLatin1String("d") || value == QLatin1String("dir"))
                parsed->shape.kind = QStringLiteral("dir");
            else
                return refuse(QStringLiteral("--type takes file or dir (f or d), not %1")
                                  .arg(spelled(value)));
        } else if (name == QLatin1String("--changed-within")) {
            QString error;
            parsed->shape.changedWithin = parseDuration(value, &error);
            if (parsed->shape.changedWithin < 0)
                return refuse(error);
        } else if (name == QLatin1String("--since")) {
            parsed->since = value;
        } else if (name == QLatin1String("--name")) {
            parsed->name = value;
        } else if (name == QLatin1String("--id")) {
            parsed->id = value;
        } else if (name == QLatin1String("--check")) {
            parsed->check = value;
        } else if (name == QLatin1String("--mount")) {
            parsed->mount = true;
        } else if (name == QLatin1String("--from")) {
            parsed->from = value;
        } else if (name == QLatin1String("--from-xattr")) {
            parsed->fromXattr = true;
        } else if (name == QLatin1String("--desktop")) {
            parsed->desktop = true;
        } else if (name == QLatin1String("--replace")) {
            parsed->replace = value;
        } else if (name == QLatin1String("--with")) {
            parsed->with = value;
        } else if (name == QLatin1String("--pattern")) {
            parsed->pattern = value;
        } else if (name == QLatin1String("--status")) {
            parsed->status = true;
        } else if (name == QLatin1String("--all")) {
            parsed->all = true;
        }
    }
    const int count = parsed->positionals.size();
    // Before the operands are counted or resolved, so `ls --untagged --any x
    // /nope` names the flag rather than sending somebody after the path.
    for (const Flag &flag : kFlags) {
        if (!given.contains(flag.name))
            continue;
        if (const QString conflict = conflictOf(named, flag.name, given, count > 0);
            !conflict.isEmpty()) {
            fail(conflict);
            return kUsage;
        }
    }
    if (takes.required && !parsed->typed(takes.required))
        return refuse(QStringLiteral("%1 is required").arg(QLatin1String(takes.required)));
    if (const Takes counted = bySet(takes, given);
        count < counted.least || (counted.most >= 0 && count > counted.most)) {
        return refuse(counted.count ? QString::fromLatin1(counted.count)
                      : counted.most == 0
                          ? QStringLiteral("takes no arguments")
                          : QStringLiteral("takes %1one path")
                                .arg(counted.least < counted.most ? QStringLiteral("at most ")
                                                                  : QString()));
    }
    return kOk;
}

// ------------------------------------------------------------------- batch

int parseCommand(const Globals &g, Session &s, QStringList rest, std::function<int()> *run);

// POSIX-ish word splitting, because the lines of a command file are mostly
// paths and real file names have spaces in them. Splitting on whitespace alone
// would be a bug shipped on purpose: `tag add --tag x -- /home/me/tax return.pdf`
// would silently become two paths, neither of which exists.
//
// Single quotes take everything to the next one literally. Double quotes do the
// same except that a backslash before `$`, a backtick, a quote or another
// backslash escapes it, as POSIX 2.2.3 has it. Unquoted, a backslash escapes
// whatever follows.
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
                    && QStringLiteral("$`\"\\").contains(line.at(i + 1)))
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
        // POSIX's default IFS past the newline that ended the line: a space or
        // a tab and nothing else, so `\r`, `\v` or U+00A0 is part of a word.
        if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
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
// `view` and `roots` write files of their own, and `sweep` a list of names and
// a receipt, one lock hold per tree: a rolled back index undoes none of those.
//
// The read verbs are refused for the other half of the promise. A batch says
// nothing until it has committed, so an `ls` in the middle of one could only
// print rows a later line might still take away.
//
// `batch` is the file the batch was read out of, told by its device and inode,
// which then holds nothing for an `import` line: `-` where that was stdin, or
// another name for it, `/dev/stdin` and `/proc/self/fd/0` among them.
bool batchTakes(const QStringList &words, const struct stat &batch, QString *why)
{
    const auto namesBatch = [&batch](const QString &word) {
        if (word != QLatin1String("-"))
            return fileIdOf(word) == fileId(batch);
        struct stat input {};
        return ::fstat(STDIN_FILENO, &input) == 0 && fileId(input) == fileId(batch);
    };
    for (const QString &word : words) {
        if (word == QLatin1String("--"))
            break;
        if (isGlobalFlag(word)) {
            const QString global = globalFlag(word);
            *why = global == QLatin1String("--help") || global == QLatin1String("--version")
                ? QStringLiteral("%1 is no part of a command; a line cannot ask for it").arg(word)
                : QStringLiteral("%1 is a flag for the batch itself, not for a line").arg(word);
            return false;
        }
    }

    const QString &cmd = words.first();
    static const QStringList filesystem = {QStringLiteral("mv"), QStringLiteral("cp"),
                                           QStringLiteral("rm"), QStringLiteral("trash"),
                                           QStringLiteral("chmod")};
    if (filesystem.contains(cmd)) {
        *why = QStringLiteral("%1 changes files on disk and a rolled back index cannot "
                              "undo that; run it on its own")
                   .arg(cmd);
        return false;
    }
    // `view` and `roots` with no subcommand, or a flag next, are their own `ls`.
    const bool files = cmd == QLatin1String("view") || cmd == QLatin1String("roots");
    const bool listing = words.value(1) == QLatin1String("ls")
        ? files || cmd == QLatin1String("tag")
        : files && (words.size() == 1 || words.at(1).startsWith(QLatin1Char('-')));
    if (onlyReads(cmd) || listing) {
        *why = QStringLiteral("%1 only reads, and a batch prints nothing until it commits; "
                              "run it on its own")
                   .arg(listing ? cmd + QStringLiteral(" ls") : cmd);
        return false;
    }
    if (files) {
        *why = QStringLiteral("%1 writes a file outside the index and a rolled back index "
                              "cannot undo that; run it on its own")
                   .arg(cmd);
        return false;
    }
    if (cmd == QLatin1String("sweep")) {
        *why = QStringLiteral("sweep takes the lock one tree at a time and keeps its lists "
                              "of names and its receipt outside the index, which one batch "
                              "commit cannot cover; run it on its own");
        return false;
    }
    if (const auto named = std::find_if(words.cbegin() + 1, words.cend(), namesBatch);
        cmd == QLatin1String("import") && named != words.cend()) {
        *why = QStringLiteral("import %1 reads the file this batch was read out of; "
                              "name the dump's file").arg(spelled(*named));
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
    const QString source = positionals.isEmpty() ? QStringLiteral("-") : positionals.first();

    QByteArray input;
    int status = readInput(QStringLiteral("batch"), source, &input);
    if (status != kOk)
        return status;
    // Asked of what was read: that it read is what makes the answer there.
    struct stat read {};
    if (source == QLatin1String("-") ? ::fstat(STDIN_FILENO, &read)
                                     : ::stat(QFile::encodeName(source).constData(), &read)) {
        fail(QStringLiteral("batch: cannot read %1: %2").arg(spelled(source), systemReason(errno)));
        return kIncomplete;
    }

    // The whole file is read before the first line runs. A command that asked
    // stdin a question halfway through would otherwise eat the lines that were
    // still waiting to be commands -- `prune` is one, so from a terminal it can
    // prompt in the middle of a run and the run survives it. Off a terminal it
    // refuses, and the batch fails with it; `--yes` on the batch answers for
    // every line at once.
    // Read whole and split here: a line read from stdin stops at a NUL, and
    // the rest of it would go unseen.
    // A byte order mark at byte 0 is the editor's, not the first command's.
    QList<QByteArray> raw = withoutBom(input).split('\n');
    // The piece after the last `\n`: a line nothing ended, or nothing at all.
    const QByteArray unended = raw.takeLast();
    if (!unended.isEmpty())
        raw.append(unended);
    QStringList lines;
    for (int number = 1; number <= raw.size(); ++number) {
        QByteArray bytes = raw.at(number - 1);
        if (bytes.endsWith('\r') && (number < raw.size() || unended.isEmpty()))
            bytes.chop(1);
        QString line;
        if (!nameFromBytes(bytes, &line)) {
            fail(QStringLiteral("batch: line %1: not valid UTF-8, or holds a NUL, so no "
                                "path here can be spelled from it; nothing was run")
                     .arg(number));
            fail(QStringLiteral("  %1").arg(escapedBytes(bytes)));
            return kUsage;
        }
        lines.append(line);
    }

    const auto refuse = [](int number, const QString &line, const QString &why) {
        fail(QStringLiteral("batch: line %1: %2").arg(number).arg(why));
        fail(QStringLiteral("  %1").arg(spelled(line)));
    };

    // Every line split, vetted and parsed before the first one runs, so a
    // refusal on line 5 comes before line 1 has done anything. Deferred first:
    // a line's parse does not decide how the batch's own session opens.
    s.deferred = true;
    struct Command {
        int index;
        QString verb;
        std::function<int()> run;
    };
    QVector<Command> commands;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        // The first column and nowhere else: past it `#` is an ordinary
        // character, which is what makes a file called `#draft.md` nameable
        // without quoting it.
        const bool blank = std::all_of(line.cbegin(), line.cend(), [](QChar c) {
            return c == QLatin1Char(' ') || c == QLatin1Char('\t');
        });
        if (blank || line.startsWith(QLatin1Char('#')))
            continue;
        QStringList words;
        QString error;
        if (!splitLine(line, &words, &error)) {
            refuse(i + 1, line, error);
            return kUsage;
        }
        QString why;
        if (!batchTakes(words, read, &why)) {
            refuse(i + 1, line, why);
            return kUsage;
        }
        std::function<int()> run;
        if ((status = parseCommand(g, s, words, &run)) != kOk) {
            refuse(i + 1, line, QStringLiteral("refused, nothing was run"));
            return status;
        }
        commands.append({i, spokenVerb(words), run});
    }
    // Nothing to run is nothing to write: the index is read, as by every verb,
    // and neither locked nor made.
    if (commands.isEmpty()) {
        s.writes = false;
        if ((status = openSession(&s)) != kOk)
            return status;
        if (g.json)
            printRows(g, {});
        return kOk;
    }

    status = openSession(&s);
    if (status != kOk)
        return status;

    for (const Command &command : std::as_const(commands)) {
        s.line = command.verb;
        status = command.run();
        if (status != kOk) {
            refuse(command.index + 1, lines.at(command.index),
                   QStringLiteral("failed, nothing was written"));
            return status;
        }
    }
    s.line.clear();

    s.deferred = false;
    const QVector<Record> rows = committedRows(s.store, s.written);
    if ((status = settle(g, s, &rows)) != kOk)
        return status;
    // A line that aborts takes the whole run back above. This is the other
    // half: a line that ran to the end and did not settle what it promised --
    // `reindex` over a refused xattr, `tag clear` on a path with no row to
    // record the drop -- exits 1 on its own, and the batch says the same, in a
    // dry run too, where it is a refusal the program made for itself.
    //
    // The commit stands, because the rows landed and a diagnosis about what did
    // not settle is no reason to throw away what did. A refused mirror that
    // `tag add` promises nothing about is exit 0 here as it is on its own.
    return settledStatus(s);
}

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
    if (parsed.typed("--replace") != parsed.typed("--with")) {
        fail(QStringLiteral("mv: --replace and --with go together; --with '' deletes "
                            "the text"));
        return kUsage;
    }
    if (parsed.typed("--replace") && parsed.replace.isEmpty()) {
        fail(QStringLiteral("mv: --replace needs some text to find"));
        return kUsage;
    }
    static const QRegularExpression token(QStringLiteral("\\{([^{}]*)\\}"));
    if (parsed.typed("--pattern")) {
        if (parsed.pattern.isEmpty()) {
            fail(QStringLiteral("mv: --pattern needs a template"));
            return kUsage;
        }
        auto found = token.globalMatch(parsed.pattern);
        while (found.hasNext()) {
            const QString name = found.next().captured(1);
            if (name != QLatin1String("name") && name != QLatin1String("stem")
                && name != QLatin1String("ext") && name != QLatin1String("n")) {
                fail(QStringLiteral("mv: --pattern: unknown token %1; the tokens are "
                                    "{name}, {stem}, {ext} and {n}")
                         .arg(spelled(QLatin1Char('{') + name + QLatin1Char('}'))));
                return kUsage;
            }
        }
    }

    int status = kOk;
    // As named, link and all, the way `mv <old> <new>` takes its source.
    const QStringList sources = resolveNamed(QStringLiteral("mv"), parsed.positionals, &status);
    refuseOperands(QStringLiteral("mv"), parsed.positionals, sources, &status);
    refuseRoots(QStringLiteral("mv"), sources, &status);
    if (status != kOk)
        return status;

    struct Plan {
        QString from;
        QString to;
        bool folder;
        QStringList stale;      // the rows at `to` and under it
    };
    QVector<Plan> plans;
    QSet<QString> claimed;
    const int width = QString::number(sources.size()).size();
    for (int i = 0; i < sources.size(); ++i) {
        const QString &from = sources.at(i);
        QString was;
        const QString folder = typedDirectory(from, &was);
        QString name = was;
        if (parsed.typed("--replace"))
            name.replace(parsed.replace, parsed.with);
        if (parsed.typed("--pattern")) {
            const qsizetype dot = suffixDot(name);
            const QString stem = dot >= 0 ? name.left(dot) : name;
            const QString ext = dot >= 0 ? name.mid(dot) : QString();
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
            fail(QStringLiteral("mv: %1 would become %2, which is not a name")
                     .arg(spelled(from), spelled(name)));
            return kUsage;
        }
        if (name == was)
            continue;
        const QString to = QDir(folder).filePath(name);
        refuseKept(QStringLiteral("mv"), {to}, &status);
        if (status != kOk)
            return status;
        if (const QString taken = occupied(QStringLiteral("mv"), to);
            !taken.isEmpty() && !recased(from, to)) {
            fail(taken);
            return kUsage;
        }
        if (claimed.contains(to)) {
            fail(QStringLiteral("mv: two of them would both become %1").arg(spelled(to)));
            return kUsage;
        }
        if (const QString refusal = cannotMove(from, to); !refusal.isEmpty()) {
            fail(refusal);
            return kUsage;
        }
        claimed.insert(to);
        plans.append({from, to, isFolder(from), {}});
    }
    // Children before their parents, `find -depth` order, so no rename moves a
    // path a later one names: the plan printed is the plan carried out.
    std::stable_sort(plans.begin(), plans.end(), [](const Plan &one, const Plan &other) {
        return one.from.count(QLatin1Char('/')) > other.from.count(QLatin1Char('/'));
    });

    if ((status = openSession(&s)) != kOk)
        return status;
    for (Plan &plan : plans) {
        QString away;
        QStringList moving;
        if (!rowsUnder(s.store, plan.from, plan.folder, &moving, &away)
            || !staleUnder(s.store, plan.from, plan.to, &plan.stale, &away)) {
            fail(QStringLiteral("mv: %1").arg(away));
            return kUsage;
        }
    }

    // A dry run prints the plan in the shape the run prints its result: the
    // old name and the new one, tab apart, one rename to a line -- the run
    // is about a set, and a destination on its own does not say which of
    // them it came from. Under --json one `from`, `to`, `rows` object per
    // rename, the object `mv <old> <new>` already answers in, in an array.
    // Both follow each rename in the index as they go, so a parent's rows are
    // the ones its children's renames left, and are printed once it lands.
    QJsonArray moved;
    QStringList lines;
    QVector<const Plan *> done;
    QVector<Record> written;
    bool refused = false;
    for (Plan &plan : plans) {
        const bool linked = rowAtLink(s, plan.from);
        QStringList tags;
        QString why;
        QString note;
        if (!landingTags(s, plan.from, &tags, &why)
            || (!g.dryRun && !moveNamed(plan.from, plan.to, tags, &why, &note))) {
            // The rest of the set is still tried, and every row of what did
            // move is still rewritten below: a rename that failed halfway has
            // still renamed things, and an index left behind the disk is the
            // state `verify` exists to shout about.
            fail(QStringLiteral("mv: cannot move %1 to %2: %3")
                     .arg(spelled(plan.from), spelled(plan.to), why));
            refused = true;
            continue;
        }
        if (!g.dryRun)
            plan.to = storedSpelling(plan.to);
        if (linked)
            dropLinkRow(g, s, plan.from);
        dropStale(g, s, QStringLiteral("mv"), plan.stale);
        if (!note.isEmpty())
            warn(g, QStringLiteral("mv: %1").arg(note));
        done.append(&plan);
        QVector<Record> rows;
        followMove(s, rowsAt(s.store, plan.from, plan.folder), plan.from, plan.to, tags, &rows);
        written += rows;
        moved.append(movedJson(plan.from, plan.to, rows));
        lines.append(plan.from + QLatin1Char('\t') + plan.to);
    }
    if (!g.dryRun) {
        if (const int committed = finish(g, s, written, false); committed != kOk) {
            // Parents before their children, the plan's order backwards.
            for (auto plan = done.crbegin(); plan != done.crend(); ++plan)
                undoMove((*plan)->from, (*plan)->to, s.store.loaded((*plan)->from).tags);
            return committed;
        }
    }
    if (g.json) {
        printJson(moved);
    } else {
        for (const QString &line : std::as_const(lines))
            out() << line << '\n';
    }
    return refused ? kUsage : kOk;
}

// The verb and its flags, parsed and checked, and the run they ask for left in
// `run`: split so a batch can check every line before the first one runs.
int parseCommand(const Globals &g, Session &s, QStringList rest, std::function<int()> *run)
{
    // The outermost call names the run; a batch line keeps `batch`.
    if (s.verb.isEmpty())
        s.verb = spokenVerb(rest);
    rest = attachedSplit(rest);
    const QString cmd = rest.takeFirst();
    // A global every verb takes except one has to be refused by that one, or it
    // is not an exception. Asked, not defaulted: `OMAFILES_JSON` stays quiet.
    if (const char *why = takesFor(cmd).noJson; why && g.jsonAsked) {
        fail(QStringLiteral("%1: --json has no meaning here; %2").arg(cmd, QLatin1String(why)));
        return kUsage;
    }

    // Decided here, once, from the verb -- not guessed inside each command.
    // `tag ls` is the one subcommand of a writing verb that only reads. A batch
    // decided for its lines before it parsed them.
    if (!s.opened && !s.deferred) {
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
        // verb below is, and `takesFor` is where it says which. One parse for
        // the whole family would let each of the six take its siblings' flags
        // and then ignore them -- `tag clear --tag drop` dropping every tag on
        // the path at exit 0 -- and `--from` eats the next word, so it would
        // swallow the `--` that separates flags from paths.
        if (!subcommandsOf(cmd).contains(sub)) {
            fail(QStringLiteral("tag: unknown subcommand %1").arg(spelled(sub)));
            return kUsage;
        }
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd, sub);
        if (parsedStatus != kOk)
            return parsedStatus;
        const QString verb = QStringLiteral("tag ") + sub;
        const QStringList &paths = parsed.positionals;
        if (sub == QLatin1String("add") || sub == QLatin1String("rm")) {
            QString error;
            const QStringList tags = normalizeTags(parsed.tags, &error);
            if (tags.isEmpty()) {
                fail(QStringLiteral("%1: %2").arg(verb, error));
                return kUsage;
            }
            *run = sub == QLatin1String("add")
                ? std::function<int()>([=, &g, &s] { return cmdTagAdd(g, s, tags, paths); })
                : std::function<int()>([=, &g, &s] { return cmdTagRm(g, s, tags, paths); });
            return kOk;
        }
        if (sub == QLatin1String("ls")) {
            *run = [=, &g, &s] { return cmdTagLs(g, s, paths); };
            return kOk;
        }
        if (sub == QLatin1String("merge") || sub == QLatin1String("rename")) {
            QString error;
            const QString from = normalizeTag(paths.at(0), &error);
            const QString to = from.isEmpty() ? QString() : normalizeTag(paths.at(1), &error);
            if (to.isEmpty()) {
                fail(QStringLiteral("%1: %2").arg(verb, error));
                return kUsage;
            }
            // A rename onto itself changes nothing and would still rewrite every
            // row and file it names, so it is refused, as `mv a a` is.
            if (from == to) {
                fail(QStringLiteral("%1: %2 and %3 are the same tag")
                         .arg(verb, spelled(paths.at(0)), spelled(paths.at(1))));
                return kUsage;
            }
            *run = [=, &g, &s] { return cmdTagMerge(g, s, from, to); };
            return kOk;
        }
        if (sub == QLatin1String("cp"))
            *run = [=, &g, &s] { return cmdTagCp(g, s, parsed.from, paths); };
        else
            *run = [=, &g, &s] { return cmdTagClear(g, s, paths); };
        return kOk;
    }
    if (cmd == QLatin1String("ls")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        QString error;
        QStringList ands;
        if (!parsed.tags.isEmpty()) {
            ands = normalizeTags(parsed.tags, &error);
            if (ands.isEmpty()) {
                fail(QStringLiteral("ls: %1").arg(error));
                return kUsage;
            }
        }
        QStringList anys;
        if (!parsed.anys.isEmpty()) {
            anys = normalizeTags(parsed.anys, &error);
            if (anys.isEmpty()) {
                fail(QStringLiteral("ls: %1").arg(error));
                return kUsage;
            }
        }
        qint64 sinceSeconds = -1;
        if (parsed.typed("--since")) {
            sinceSeconds = parseDuration(parsed.since, &error);
            if (sinceSeconds < 0) {
                fail(QStringLiteral("ls: %1").arg(error));
                return kUsage;
            }
        }
        if (parsed.name.trimmed().isEmpty() && !parsed.name.isNull()) {
            fail(QStringLiteral("ls: --name %1 needs a pattern, like '*.pdf'")
                     .arg(spelled(parsed.name)));
            return kUsage;
        }
        // GNU `find -name` warns on the same pattern.
        if (parsed.name.contains(QLatin1Char('/'))) {
            fail(QStringLiteral("ls: --name %1 can never match; it fits the name alone, and "
                                "a name holds no /, so narrow the folder with [dir]")
                     .arg(spelled(parsed.name)));
            return kUsage;
        }
        *run = [=, &g, &s] {
            return cmdLs(g, s, ands, anys, parsed.untagged, parsed.cached, parsed.name,
                         parsed.exts, parsed.shape, sinceSeconds, parsed.positionals);
        };
        return kOk;
    }
    if (cmd == QLatin1String("tags")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        QStringList scope;
        if (!parsed.tags.isEmpty()) {
            QString error;
            scope = normalizeTags(parsed.tags, &error);
            if (scope.isEmpty()) {
                fail(QStringLiteral("tags: %1").arg(error));
                return kUsage;
            }
        }
        *run = [=, &g, &s] { return cmdTags(g, s, scope, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("prune")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdPrune(g, s, parsed.all, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("open")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdOpen(g, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("has")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdHas(g, s, parsed.tags, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("json")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdJson(g, s, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("export")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdExport(g, s); };
        return kOk;
    }
    if (cmd == QLatin1String("import")) {
        Parsed parsed;
        if (const int parsedStatus = parseFlags(rest, &parsed, cmd); parsedStatus != kOk)
            return parsedStatus;
        const QString source = parsed.positionals.first();
        *run = [=, &g, &s] { return cmdImport(g, s, source); };
        return kOk;
    }
    if (cmd == QLatin1String("mv")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        if (renamesSet(parsed.given)) {
            *run = [=, &g, &s] { return cmdRenameSet(g, s, parsed); };
            return kOk;
        }
        *run = [=, &g, &s] { return cmdMove(g, s, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("cp")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdCopy(g, s, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("trash") || cmd == QLatin1String("rm")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] {
            return cmdRemove(g, s, parsed.positionals, cmd == QLatin1String("trash"));
        };
        return kOk;
    }
    if (cmd == QLatin1String("chmod")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdChmod(g, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("reindex")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdReindex(g, s, parsed.fromXattr, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("sweep")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdSweep(g, s, parsed.status, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("view") || cmd == QLatin1String("roots")) {
        const QString sub = rest.isEmpty() || rest.first().startsWith(QLatin1Char('-'))
            ? QStringLiteral("ls")
            : rest.takeFirst();
        if (!subcommandsOf(cmd).contains(sub)) {
            fail(QStringLiteral("%1: unknown subcommand %2").arg(cmd, spelled(sub)));
            return kUsage;
        }
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd, sub);
        if (parsedStatus != kOk)
            return parsedStatus;
        if (cmd == QLatin1String("roots")) {
            *run = [=, &g, &s] {
                const auto check = parsed.typed("--check") ? std::optional(parsed.check)
                                                           : std::nullopt;
                return cmdRoots(g, s, sub, parsed.id, parsed.mount, check, parsed.positionals);
            };
            return kOk;
        }
        QStringList tags;
        if (!parsed.tags.isEmpty()) {
            QString error;
            tags = normalizeTags(parsed.tags, &error);
            if (tags.isEmpty()) {
                fail(QStringLiteral("view %1: %2").arg(sub, error));
                return kUsage;
            }
        }
        *run = [=, &g, &s] { return cmdView(g, s, sub, parsed.name, tags, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("verify")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdVerify(g, s, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("batch")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdBatch(g, s, parsed.positionals); };
        return kOk;
    }

    if (cmd == QLatin1String("recent")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=, &g, &s] { return cmdRecent(g, s, parsed.desktop, parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("completions")) {
        Parsed parsed;
        const int parsedStatus = parseFlags(rest, &parsed, cmd);
        if (parsedStatus != kOk)
            return parsedStatus;
        *run = [=] { return cmdCompletions(parsed.positionals); };
        return kOk;
    }
    if (cmd == QLatin1String("__complete")) {
        // Everything after `--` is the shell's words, verbatim, including a
        // trailing empty one when the cursor sits on a fresh argument.
        QStringList words = rest;
        if (!words.isEmpty() && words.first() == QLatin1String("--"))
            words.removeFirst();
        *run = [=, &g, &s] { return cmdComplete(words); };
        return kOk;
    }

    // Not one of the verbs above. A word before the verb that is not a global
    // spelled whole -- `--json=1`, `-jn`, a verb's own `--tag` -- is a flag; a
    // bare `-` is an operand, as `git -` says.
    const QString refused = valueRefused(cmd);
    fail(!refused.isEmpty() ? QStringLiteral("omafiles: %1").arg(refused)
         : cmd.startsWith(QLatin1Char('-')) && cmd != QLatin1String("-")
             ? QStringLiteral("omafiles: %1 is not a global flag; a verb's own flags go "
                              "after it (try omafiles --help)").arg(spelled(cmd))
             : QStringLiteral("omafiles: unknown command: %1 (try omafiles --help)")
                   .arg(spelled(cmd)));
    return kUsage;
}

int dispatch(const Globals &g, Session &s, const QStringList &rest)
{
    std::function<int()> run;
    const int status = parseCommand(g, s, rest, &run);
    return status == kOk ? run() : status;
}

} // namespace

int main(int argc, char **argv)
{
    // gnulib's `stdopen`: a standard descriptor that is closed is opened on
    // /dev/null the wrong way round, so no file this run opens can become it and
    // a write to a closed stdout still fails, with EBADF.
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1 || errno != EBADF)
            continue;
        if (::open("/dev/null", fd == STDIN_FILENO ? O_WRONLY : O_RDONLY) != fd)
            return kUsage;
        if (fd == STDIN_FILENO)
            stdinClosed = true;
    }
    // The bytes, before Qt decodes them into U+FFFD. A word that is not UTF-8
    // is refused rather than decoded: the text Qt makes of it names some other
    // file, and the verb would act on that one.
    QStringList args;
    QString unspelled;
    const bool nameable = argumentsFromBytes(argc, argv, &args, &unspelled);

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("omafiles"));
    QCoreApplication::setApplicationVersion(QStringLiteral(OMAFILES_VERSION));

    Globals g;
    QString unflagged;
    const bool flagged = envFlag("OMAFILES_JSON", &g.json, &unflagged)
        && envFlag("OMAFILES_DRY_RUN", &g.dryRun, &unflagged)
        && envFlag("OMAFILES_YES", &g.yes, &unflagged);

    if (!nameable || !flagged) {
        fail(QStringLiteral("omafiles: %1; nothing was run").arg(nameable ? unflagged : unspelled));
        return finished(QString(), kUsage);
    }
    QStringList said;
    QStringList rest = withoutGlobals(args, &said);
    for (const QString &global : std::as_const(said))
        applyGlobal(&g, global);

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
        if (about == QLatin1String("__complete")) {
            fail(QStringLiteral("__complete is the completion shim's protocol, and --help "
                                "does not describe it; see omafiles completions --help"));
            return finished(about, kUsage);
        }
        if (!about.isEmpty() && !printHelp(about)) {
            // What was asked about, whole: reporting only the first word says
            // `unknown command: tag`, naming a command that exists and hiding
            // the word that did not.
            fail(QStringLiteral("omafiles: unknown command: %1 (try omafiles --help)")
                     .arg(spelled(about)));
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
    //
    // The current directory is an input of the same kind: a relative path and
    // every defaulted `[dir]` are found through it, and git refuses to run from
    // one it cannot read. `__complete` has no opinion there rather than a
    // diagnostic, and `completions` prints a script no declaration changes.
    const QString verb = rest.first();
    const bool completing = verb == QLatin1String("__complete");
    QString unnameable;
    const bool environment = environmentNameable(&unnameable);
    if (!environment || !workingDirectoryNameable(&unnameable)) {
        if (!completing)
            fail(QStringLiteral("omafiles: %1; %2")
                     .arg(unnameable, environment
                                          ? QStringLiteral("a relative path is found through it")
                                          : QStringLiteral("every file this reads is found "
                                                           "through it")));
        return finished(spokenVerb(rest), kUsage);
    }
    // Completion goes on without them: only the candidates read out of the
    // declarations, or out of rows placed by one, have nothing to offer.
    QString rootsError;
    if (!loadRoots(&rootsError) && verb != QLatin1String("completions") && !completing) {
        fail(rootsError);
        return finished(spokenVerb(rest), kStore);
    }

    Session session;
    return finished(spokenVerb(rest), dispatch(g, session, rest));
}
