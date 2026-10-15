#include "Paths.h"

#include "Roots.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QUrl>
#include <QScopeGuard>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace omafiles {

namespace {

struct BaseDir {
    const char *variable;
    const char *fallback;
};
constexpr BaseDir kData{"XDG_DATA_HOME", "/.local/share"};
constexpr BaseDir kConfig{"XDG_CONFIG_HOME", "/.config"};
constexpr BaseDir kCache{"XDG_CACHE_HOME", "/.cache"};
constexpr BaseDir kState{"XDG_STATE_HOME", "/.local/state"};

// A relative XDG value counts as unset: the XDG Base Directory spec calls it
// invalid and says to ignore it. `HOME` then decides, and has to be absolute
// for the same reason: a base directory that moves with the cwd is no base.
QString baseDir(const BaseDir &base, QString *error)
{
    QByteArray value = qgetenv(base.variable);
    if (!value.startsWith('/'))
        value.clear();
    const bool fromHome = value.isEmpty();
    const char *deciding = fromHome ? "HOME" : base.variable;
    const QByteArray bytes = fromHome ? qgetenv("HOME") : value;
    QString name;
    if (!nameFromBytes(bytes, &name)) {
        if (error)
            *error = QStringLiteral("%1 is not valid UTF-8: %2")
                         .arg(QLatin1String(deciding), escapedBytes(bytes));
    } else if (fromHome && !qEnvironmentVariableIsSet("HOME")) {
        if (error)
            *error = QStringLiteral("HOME is not set");
    } else if (!name.startsWith(QLatin1Char('/'))) {
        if (error)
            *error = QStringLiteral("HOME is not an absolute path: %1").arg(spelled(name));
    }
    return fromHome ? name + QLatin1String(base.fallback) : name;
}

} // namespace

QString dataHome()
{
    return baseDir(kData, nullptr);
}

QString configHome()
{
    return baseDir(kConfig, nullptr);
}

QString cacheHome()
{
    return baseDir(kCache, nullptr);
}

QString stateHome()
{
    return baseDir(kState, nullptr);
}

bool environmentNameable(QString *error)
{
    QString said;
    for (const BaseDir &base : {kData, kConfig, kCache, kState}) {
        baseDir(base, &said);
        if (!said.isEmpty()) {
            if (error)
                *error = said;
            return false;
        }
    }
    return true;
}

namespace {

// `getcwd` and not `QDir::currentPath`, which answers "" for a directory that
// was deleted and U+FFFD for bytes that are not UTF-8.
QString workingDirectory(QString *error)
{
    char *bytes = ::getcwd(nullptr, 0);
    if (!bytes) {
        if (error)
            *error = QStringLiteral("the current directory cannot be read: %1")
                         .arg(systemReason(errno));
        return {};
    }
    const QByteArray here(bytes);
    std::free(bytes);
    QString name;
    if (nameFromBytes(here, &name))
        return name;
    if (error)
        *error = QStringLiteral("the current directory is not valid UTF-8: %1")
                     .arg(escapedBytes(here));
    return {};
}

} // namespace

bool workingDirectoryNameable(QString *error)
{
    return !workingDirectory(error).isEmpty();
}

QString homeDirectory()
{
    QString home;
    if (!nameFromBytes(qgetenv("HOME"), &home) || !home.startsWith(QLatin1Char('/')))
        return QStringLiteral("/");
    return home;
}

QString anchoredPath(const QString &path)
{
    if (path.startsWith(QLatin1Char('/')))
        return path;
    const QString here = workingDirectory(nullptr);
    if (here.isEmpty())
        return {};
    return here == QLatin1String("/") ? here + path : here + QLatin1Char('/') + path;
}

QString localFileOf(const QUrl &url)
{
    // `file://localhost/p` is local too (RFC 8089). `toLocalFile` spells a byte
    // that is not UTF-8 as U+FFFD, which is the name of some other file. A `?`
    // or `#` begins a query or a fragment, which Qt and GLib drop: another file.
    const QString host = url.host();
    const QByteArray escaped = url.path(QUrl::FullyEncoded).toLatin1();
    QString local;
    if (!url.isLocalFile() || url.hasQuery() || url.hasFragment()
        || (!host.isEmpty() && host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) != 0)
        || escaped.toLower().contains("%2f")
        || !nameFromBytes(QByteArray::fromPercentEncoding(escaped), &local))
        return {};
    return local;
}

QString defaultIndexPath()
{
    return QDir(dataHome()).filePath(QStringLiteral("omafiles/index.jsonl"));
}

bool nameFromBytes(const QByteArray &bytes, QString *name)
{
    // `QFile::decodeName` drops a leading U+FEFF, and a name can begin with one.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::ConvertInitialBom);
    const QString text = decoder(bytes);
    if (bytes.contains('\0') || QFile::encodeName(text) != bytes)
        return false;
    if (name)
        *name = text;
    return true;
}

bool argumentsFromBytes(int argc, char **argv, QStringList *words, QString *error)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray bytes(argv[i]);
        QString word;
        if (!nameFromBytes(bytes, &word)) {
            if (error)
                *error = QStringLiteral("%1 is not valid UTF-8, so no path, tag or name "
                                        "here can be spelled from it")
                             .arg(escapedBytes(bytes));
            return false;
        }
        words->append(word);
    }
    return true;
}

QByteArray withoutBom(const QByteArray &bytes)
{
    return bytes.startsWith("\xef\xbb\xbf") ? bytes.mid(3) : bytes;
}

QString systemReason(int failure)
{
    return QString::fromLocal8Bit(strerror(failure));
}

QString surrogateEscaped(const QByteArray &bytes)
{
    QString text;
    for (qsizetype i = 0; i < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes.at(i));
        const qsizetype length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        QString decoded;
        if (lead != 0 && nameFromBytes(bytes.mid(i, length), &decoded)) {
            text += decoded;
            i += length;
        } else {
            text += lead == 0 ? QChar(0) : QChar(0xdc00 + lead);
            ++i;
        }
    }
    return text;
}

namespace {

// The byte `surrogateEscaped` kept at `text[i]`, NUL included, or -1 where
// that is a character.
int escapedByte(const QString &text, qsizetype i)
{
    const QChar c = text.at(i);
    if (c.unicode() == 0)
        return 0;
    if (c.unicode() >= 0xdc80 && c.unicode() <= 0xdcff
        && (i == 0 || !text.at(i - 1).isHighSurrogate()))
        return c.unicode() - 0xdc00;
    return -1;
}

// The bytes of `text[i]` no message can show as a character, or none where it
// shows: a byte `surrogateEscaped` kept, and a control -- C0, DEL or C1, which
// coreutils' quotearg reads as unprintable too -- as its UTF-8.
QByteArray unshown(const QString &text, qsizetype i)
{
    if (const int byte = escapedByte(text, i); byte >= 0)
        return QByteArray(1, static_cast<char>(byte));
    const char16_t c = text.at(i).unicode();
    return c < 0x20 || (c >= 0x7f && c <= 0x9f) ? QString(text.at(i)).toUtf8() : QByteArray();
}

// `bytes` as `\xHH` each, the one spelling of a byte a message cannot show.
QString hexBytes(const QByteArray &bytes)
{
    QString text;
    for (const char byte : bytes)
        text += QStringLiteral("\\x%1").arg(static_cast<unsigned char>(byte), 2, 16,
                                              QLatin1Char('0'));
    return text;
}

} // namespace

QString escapedWords(const QString &text, bool bytesOnly)
{
    QString escaped;
    escaped.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (bytesOnly) {
            const int byte = escapedByte(text, i);
            escaped += byte >= 0 ? hexBytes(QByteArray(1, static_cast<char>(byte))) : QString(c);
        } else if (c == QLatin1Char('\\')) {
            escaped += QLatin1String("\\\\");
        } else if (c == QLatin1Char('\t')) {
            escaped += QLatin1String("\\t");
        } else if (c == QLatin1Char('\n')) {
            escaped += QLatin1String("\\n");
        } else if (const QByteArray bytes = unshown(text, i); !bytes.isEmpty()) {
            escaped += hexBytes(bytes);
        } else {
            escaped += c;
        }
    }
    return escaped;
}

QString escapedBytes(const QByteArray &bytes)
{
    return escapedWords(surrogateEscaped(bytes));
}

QString spelled(const QString &word)
{
    // A space `escapedWords` leaves as it is would be invisible at an end; a
    // control one, CR or tab, is spelled already.
    const auto blank = [](QChar c) {
        return c.isSpace() && c.category() != QChar::Other_Control;
    };
    const QString escaped = escapedWords(word);
    const bool bare = !word.isEmpty() && !blank(word.front()) && !blank(word.back());
    return bare ? escaped : QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

namespace {

// Which of the two answers a failed syscall was: ENOENT is "no such name", and
// every other failure -- EACCES, ENOTDIR, a dead NFS mount's ESTALE -- is the
// system declining to answer, which is not a no.
PathState stateForErrno(int failure)
{
    return failure == ENOENT ? PathState::Missing : PathState::Unknown;
}

QString systemSaid(const QString &path, int failure)
{
    return QStringLiteral("%1: %2").arg(spelled(path), systemReason(failure));
}

// The directory a path names its last component in, as typed.
QByteArray parentBytes(const QByteArray &path)
{
    const qsizetype cut = path.lastIndexOf('/');
    return cut < 0 ? QByteArrayLiteral(".") : cut == 0 ? QByteArrayLiteral("/") : path.left(cut);
}

// `path` absolute, with its deepest ancestor that resolves resolved by the
// kernel and the rest appended as typed. Never cleaned as text: `L/..` is the
// parent of where `L` leads. Empty where the working directory is gone.
QByteArray physicalBytes(QByteArray head)
{
    if (!head.startsWith('/')) {
        char *here = ::getcwd(nullptr, 0);
        if (!here)
            return {};
        head = QByteArray(here) + '/' + head;
        std::free(here);
    }
    QByteArray tail;
    while (true) {
        if (char *real = ::realpath(head.constData(), nullptr)) {
            QByteArray resolved(real);
            std::free(real);
            if (tail.isEmpty())
                return resolved;
            return resolved.endsWith('/') ? resolved + tail : resolved + '/' + tail;
        }
        if (head == "/")
            return head + tail;
        const qsizetype cut = head.lastIndexOf('/');
        const QByteArray name = head.mid(cut + 1);
        if (!name.isEmpty())
            tail = tail.isEmpty() ? name : name + '/' + tail;
        head = parentBytes(head);
    }
}

// The longest leading run of whole components that is text. No root lies
// below a name that is not, so it is the part a root lookup can use.
QString nameablePrefix(const QByteArray &bytes)
{
    QString name;
    for (qsizetype end = bytes.size(); end > 0; end = bytes.lastIndexOf('/', end - 1)) {
        if (nameFromBytes(bytes.left(end), &name))
            return name;
    }
    return QStringLiteral("/");
}

// The root a path typed through a declared root's path is under, whatever it
// resolves to now: the declaration wins over the disk.
PathState declaredStorage(const QString &path, QString *error)
{
    if (roots().isEmpty())
        return PathState::Present;
    const QString typed = cleanedAsText(anchoredPath(path));
    return typed.isEmpty() ? PathState::Present : storageState(typed, error);
}

// The root half of `pathState` for a path as typed, declared and then resolved.
// Resolving it costs a `realpath`, which a machine that declares no root never needs.
PathState storageOfTyped(const QString &path, QString *error)
{
    if (roots().isEmpty())
        return PathState::Present;
    if (const PathState declared = declaredStorage(path, error); declared != PathState::Present)
        return declared;
    const QString physical = physicalPath(path);
    return physical.isEmpty() ? PathState::Present : storageState(physical, error);
}

} // namespace

QString physicalPath(const QString &path)
{
    const QByteArray physical = physicalBytes(QFile::encodeName(path));
    return physical.isEmpty() ? QString() : nameablePrefix(physical);
}

namespace {

// The same answer, for a directory that would not open: an away root's tree is
// `ENOENT` from here, and telling that from a deletion is what a root is for.
PathState stateForErrno(int failure, const QString &path)
{
    const PathState plain = stateForErrno(failure);
    if (plain == PathState::Missing
        && storageOfTyped(path, nullptr) == PathState::Unreachable)
        return PathState::Unreachable;
    return plain;
}

} // namespace

QString danglingChain(const QString &path)
{
    // The first component that does not resolve: a link met in the middle of
    // the path is as much the one to repoint as a link at its end.
    QString said = spelled(path);
    QByteArray hop = QFile::encodeName(path);
    for (qsizetype end = path.indexOf(QLatin1Char('/'), 1); end > 0;
         end = path.indexOf(QLatin1Char('/'), end + 1)) {
        const QByteArray prefix = QFile::encodeName(path.left(end));
        char *real = ::realpath(prefix.constData(), nullptr);
        if (!real) {
            said = QStringLiteral("%1: %2").arg(spelled(path), spelled(path.left(end)));
            hop = prefix;
            break;
        }
        std::free(real);
    }
    struct stat st {};
    int hops = 0;
    // The kernel's own limit, so a chain it would follow is followed here.
    for (; hops < 40 && ::lstat(hop.constData(), &st) == 0 && S_ISLNK(st.st_mode); ++hops) {
        const QByteArray target = linkTarget(hop);
        if (target.isEmpty())
            break;
        hop = physicalBytes(target.startsWith('/') ? target : parentBytes(hop) + '/' + target);
        said += QStringLiteral(" is a symlink to %1, which").arg(escapedBytes(hop));
    }
    return hops == 0 ? QString() : said + QStringLiteral(" is not there");
}

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

namespace {

QString entryIn(const QString &dir, const QString &name)
{
    return dir == QLatin1String("/") ? dir + name : dir + '/' + name;
}

QByteArray entryPath(const QString &dir, const QString &name)
{
    return QFile::encodeName(entryIn(dir, name));
}

QString otherCase(const QString &name)
{
    QString swapped = name;
    for (QChar &c : swapped)
        c = c.isUpper() ? c.toLower() : c.toUpper();
    return swapped;
}

bool sameFile(const struct stat &one, const struct stat &other)
{
    return one.st_dev == other.st_dev && one.st_ino == other.st_ino;
}

// Whether `dir` reaches its entry `name`, read into `typed`, by the other case
// of every letter, as the same file. Nothing where `name` has no letter or is
// not there, which cannot tell.
std::optional<bool> foldsAt(const QString &dir, const QString &name, struct stat *typed)
{
    const QString swapped = otherCase(name);
    if (swapped == name || ::lstat(entryPath(dir, name).constData(), typed) != 0)
        return std::nullopt;
    struct stat other {};
    return ::lstat(entryPath(dir, swapped).constData(), &other) == 0 && sameFile(*typed, other);
}

// `name` as `dir` stores it. Linux realpath(3) keeps the spelling typed, and a
// directory that folds case (vfat, exfat, casefold) finds the file by another
// or by an alias -- an 8.3 short name, a trailing dot, another normalization;
// Darwin's realpath(3), Windows' GetFinalPathNameByHandle and Samba's mangled
// names answer with the stored name, and so does this, from the entry that is
// the same file: one alike first, then any.
QString storedName(Spellings::Dir &dir, const QString &name)
{
    struct stat typed {};
    bool read = false;
    if (!dir.folds) {
        // One name there tells for the whole directory.
        dir.folds = foldsAt(dir.stored, name, &typed);
        if (!dir.folds)
            return name;
        read = true;
    }
    if (!*dir.folds)
        return name;
    if (dir.held.isEmpty()) {
        QStringList names;
        readDirectory(dir.stored, &names, nullptr, nullptr);
        for (const QString &entry : std::as_const(names))
            dir.held[foldedName(entry)].append(entry);
    }
    const QString key = foldedName(name);
    if (dir.held.value(key).contains(name)
        || (!read && ::lstat(entryPath(dir.stored, name).constData(), &typed) != 0))
        return name;
    const auto sameAs = [&](const QStringList &spellings) {
        struct stat other {};
        for (const QString &stored : spellings) {
            if (::lstat(entryPath(dir.stored, stored).constData(), &other) == 0
                && sameFile(typed, other))
                return stored;
        }
        return QString();
    };
    QString stored = sameAs(dir.held.value(key));
    for (auto held = dir.held.cbegin(); stored.isEmpty() && held != dir.held.cend(); ++held) {
        if (held.key() != key)
            stored = sameAs(held.value());
    }
    return stored.isEmpty() ? name : stored;
}

} // namespace

QString foldedName(const QString &name)
{
    return name.normalized(QString::NormalizationForm_C).toCaseFolded();
}

QString storedSpelling(const QString &path, Spellings *known)
{
    QString name;
    const QString parent = typedDirectory(path, &name);
    if (name.isEmpty() || parent == path)
        return name.isEmpty() ? QStringLiteral("/") : path;
    Spellings once;
    Spellings &learnt = known ? *known : once;
    auto dir = learnt.dirs.find(parent);
    if (dir == learnt.dirs.end())
        dir = learnt.dirs.insert(parent, {storedSpelling(parent, &learnt), {}, {}});
    if (dir->folds == false && dir->stored == parent)
        return path;
    return entryIn(dir->stored, storedName(*dir, name));
}

bool foldsCase(const QString &dir)
{
    QStringList names;
    if (!readDirectory(dir, &names, nullptr, nullptr))
        return false;
    // Two names one case apart are two files: a directory that folds holds one.
    const QSet<QString> held(names.cbegin(), names.cend());
    for (const QString &name : std::as_const(names)) {
        struct stat typed {};
        if (held.contains(otherCase(name)))
            continue;
        if (const std::optional<bool> folds = foldsAt(dir, name, &typed))
            return *folds;
    }
    return false;
}

QString realPath(const QString &path, QString *error, PathState *state, bool *unnameable)
{
    if (unnameable)
        *unnameable = false;
    char *real = ::realpath(QFile::encodeName(path).constData(), nullptr);
    if (!real) {
        const int failure = errno;
        // A root that is away wins over whatever the kernel said about its path.
        QString detail;
        PathState found = storageOfTyped(path, &detail);
        struct stat st {};
        if (found == PathState::Present && failure == ENOENT
            && ::stat(QFile::encodeName(path).constData(), &st) == 0) {
            // A `/proc/<pid>/fd` magic link to a pipe, a socket or a deleted file:
            // the kernel reaches it and `realpath` finds no name for it.
            found = PathState::Unknown;
            detail = QStringLiteral("%1 is %2 with no name on a filesystem")
                         .arg(spelled(path), S_ISFIFO(st.st_mode)   ? QStringLiteral("a pipe")
                                             : S_ISSOCK(st.st_mode) ? QStringLiteral("a socket")
                                                                    : QStringLiteral("a file"));
        } else if (found == PathState::Present) {
            found = stateForErrno(failure);
            detail = found == PathState::Missing ? danglingChain(path) : systemSaid(path, failure);
            if (detail.isEmpty())
                detail = QStringLiteral("path missing: %1").arg(spelled(path));
        }
        if (state)
            *state = found;
        if (error)
            *error = detail;
        return {};
    }
    const QByteArray resolved(real);
    std::free(real);
    QString name;
    if (!nameFromBytes(resolved, &name)) {
        if (unnameable)
            *unnameable = true;
        if (state)
            *state = PathState::Unknown;
        if (error)
            *error = QStringLiteral("%1: leads to %2, which is not valid UTF-8, so no path "
                                    "here can be spelled from it")
                         .arg(spelled(path), escapedBytes(resolved));
        return {};
    }
    if (state)
        *state = PathState::Present;
    return storedSpelling(name);
}

QString canonicalPath(const QString &path, QString *error, PathState *state)
{
    const QString resolved = realPath(path, error, state);
    if (resolved.isEmpty())
        return {};
    PathState storage = declaredStorage(path, error);
    if (storage == PathState::Present)
        storage = storageState(resolved, error);
    if (state)
        *state = storage;
    return storage == PathState::Present ? resolved : QString();
}

QString canonicalLinkPath(const QString &path, QString *error, PathState *state)
{
    // Only a symlink differs; everything else resolves through `canonicalPath`,
    // so two spellings of one directory cannot become two index rows.
    struct stat st {};
    if (statState(path, nullptr, false, &st) != PathState::Present || !S_ISLNK(st.st_mode))
        return canonicalPath(path, error, state);
    // The prefix resolves; only the name the link is filed under stays.
    QString name;
    const QString parent = canonicalPath(typedDirectory(path, &name), error, state);
    if (parent.isEmpty())
        return {};
    Spellings::Dir at{parent, {}, {}};
    const QString linked = QDir(parent).filePath(storedName(at, name));
    const PathState storage = storageState(linked, error);
    if (state)
        *state = storage;
    return storage == PathState::Present ? linked : QString();
}

QString typedDirectory(const QString &path, QString *name)
{
    QString trimmed = path;
    while (trimmed.size() > 1 && trimmed.endsWith(QLatin1Char('/')))
        trimmed.chop(1);
    const qsizetype cut = trimmed.lastIndexOf(QLatin1Char('/'));
    if (name)
        *name = trimmed == QLatin1String("/") ? QString() : trimmed.mid(cut + 1);
    if (cut < 0)
        return QStringLiteral(".");
    return cut == 0 ? QStringLiteral("/") : trimmed.left(cut);
}

bool isCleanRelative(const QString &path)
{
    return !path.isEmpty() && !QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path
        && path != QLatin1String("..") && !path.startsWith(QLatin1String("../"));
}

bool isCleanAbsolute(const QString &path)
{
    return QDir::isAbsolutePath(path) && QDir::cleanPath(path) == path;
}

QString shellQuoted(const QString &text)
{
    static const QRegularExpression plain(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_@%+=:,./-]+")));
    if (plain.match(text).hasMatch())
        return text;
    // `'"'"'` for a quote rather than `'\''`, so a reader that doubles
    // backslashes -- `verify`'s detail field -- leaves the command whole. What
    // no message shows is `$'\xHH'` between quotes, coreutils' shell-escape
    // style, so the word stays on one line.
    QString quoted = QStringLiteral("'");
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\''))
            quoted += QLatin1String("'\"'\"'");
        else if (const QByteArray bytes = unshown(text, i); !bytes.isEmpty())
            quoted += QStringLiteral("'$'%1''").arg(hexBytes(bytes));
        else
            quoted += c;
    }
    return quoted + QLatin1Char('\'');
}

bool isUnderDirectory(const QString &path, const QString &directory)
{
    if (path == directory)
        return true;
    const QString prefix = directory.endsWith(QLatin1Char('/')) ? directory
                                                                : directory + QLatin1Char('/');
    return path.startsWith(prefix);
}

QString cleanedAsText(const QString &path)
{
    return path.split(QLatin1Char('/')).contains(QLatin1String("..")) ? QString()
                                                                      : QDir::cleanPath(path);
}

QString relativeTo(const QString &path, const QString &directory)
{
    if (path == directory)
        return QStringLiteral(".");
    return path.mid(directory.endsWith(QLatin1Char('/')) ? directory.size() : directory.size() + 1);
}

PathState storageState(const QString &path, QString *error)
{
    const auto root = rootFor(path);
    if (!root)
        return PathState::Present;
    QString why;
    const PathState storage = rootState(*root, &why);
    if (error && storage == PathState::Unreachable)
        *error = unreachableReason(path, *root);
    else if (error && storage == PathState::Unknown)
        *error = QStringLiteral("%1: root %2 cannot be checked: %3")
                     .arg(spelled(path), root->id, why);
    return storage;
}

PathState pathState(const QString &path, QString *error)
{
    const PathState storage = storageOfTyped(path, error);
    return storage == PathState::Present ? statState(path, error) : storage;
}

PathState rowState(const QString &path, QString *error)
{
    // A row is resolved already, so its root is looked up as it stands.
    const PathState storage = storageState(path, error);
    if (storage != PathState::Present)
        return storage;
    // A row is the resolved path of a file, so it counts only where resolving it
    // gives it back: a symlink at it or above it leads to some other file.
    const QByteArray bytes = QFile::encodeName(path);
    char *real = ::realpath(bytes.constData(), nullptr);
    if (!real) {
        const int failure = errno;
        if (error)
            *error = systemSaid(path, failure);
        return failure == ENOTDIR || failure == ELOOP ? PathState::Missing
                                                       : stateForErrno(failure);
    }
    const QByteArray resolved(real);
    std::free(real);
    if (resolved == bytes)
        return PathState::Present;
    if (error)
        *error = QStringLiteral("%1 leads through a symlink to %2 now")
                     .arg(spelled(path), escapedBytes(resolved));
    return PathState::Missing;
}

bool leadsNowhere(const QString &path)
{
    struct stat st {};
    return ::stat(QFile::encodeName(path).constData(), &st) != 0
        && (errno == ENOENT || errno == ENOTDIR);
}

PathState statState(const QString &path, QString *error, bool followLinks, struct stat *found)
{
    struct stat st {};
    struct stat *into = found ? found : &st;
    const QByteArray name = QFile::encodeName(path);
    if ((followLinks ? ::stat(name.constData(), into) : ::lstat(name.constData(), into)) == 0)
        return PathState::Present;
    // Kept before anything else can overwrite it: building the message below
    // calls into Qt, and errno belongs to the stat.
    const int failure = errno;
    if (error)
        *error = systemSaid(path, failure);
    return stateForErrno(failure);
}

PathState openToRead(QFile *file, QIODevice::OpenMode mode, QString *error, Reading kind)
{
    // Opened here rather than by QFile, whose failure keeps no errno to judge.
    // A QFile opened on a descriptor forgets its name, so a caller keeps it.
    // A kept file never blocks: `O_NONBLOCK` opens a fifo at once, and only a
    // regular file is read.
    const QString path = file->fileName();
    const int flags = O_RDONLY | O_CLOEXEC | (kind == Reading::Kept ? O_NONBLOCK : 0);
    const int fd = ::open(QFile::encodeName(path).constData(), flags);
    int failure = fd < 0 ? errno : 0;
    struct stat st {};
    if (fd >= 0 && ::fstat(fd, &st) != 0)
        failure = errno;
    else if (fd >= 0 && S_ISDIR(st.st_mode))
        failure = EISDIR;
    const bool irregular = failure == 0 && kind == Reading::Kept && !S_ISREG(st.st_mode);
    if (failure != 0 || irregular) {
        if (fd >= 0)
            ::close(fd);
        if (error)
            *error = irregular ? QStringLiteral("%1: is not a regular file").arg(spelled(path))
                               : systemSaid(path, failure);
        return irregular ? PathState::Unknown : stateForErrno(failure);
    }
    if (!file->open(fd, mode | QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        if (error)
            *error = QStringLiteral("%1: %2").arg(spelled(path), file->errorString());
        return PathState::Unknown;
    }
    return PathState::Present;
}

bool makePath(const QString &directory, QString *error)
{
    for (qsizetype end = directory.indexOf(QLatin1Char('/'), 1);;
         end = directory.indexOf(QLatin1Char('/'), end + 1)) {
        const QString prefix = end < 0 ? directory : directory.left(end);
        if (::mkdir(QFile::encodeName(prefix).constData(), 0777) != 0 && errno != EEXIST) {
            const int failure = errno;
            if (error)
                *error = systemSaid(prefix, failure);
            return false;
        }
        if (end < 0)
            return true;
    }
}

bool readDirectory(const QString &directory, QStringList *names, QStringList *unnameable,
                   QString *error, PathState *state)
{
    return visitDirectory(directory, [names](QAnyStringView name, unsigned char) {
        if (names)
            names->append(name.toString());
        return true;
    }, unnameable, error, state);
}

namespace {

bool visitEntries(DIR *handle, const QString &directory,
                    const std::function<bool(QAnyStringView, unsigned char)> &visitor,
                    QStringList *unnameable, QString *error, PathState *state)
{
    if (!handle) {
        const int failure = errno;
        if (error)
            *error = systemSaid(directory, failure);
        if (state)
            *state = stateForErrno(failure, directory);
        return false;
    }
    if (state)
        *state = PathState::Present;
    while (true) {
        // Zeroed before every read, not once before the loop: `readdir` returns
        // null for the end of the directory and for a failure alike, errno is
        // the only thing that tells them apart, and anything the body calls may
        // have written to it in between.
        errno = 0;
        const dirent *entry = ::readdir(handle);
        if (!entry)
            break;
        const QByteArrayView bytes(entry->d_name);
        if (bytes == "." || bytes == "..")
            continue;
        QString decoded;
        QAnyStringView name;
        if (std::all_of(bytes.begin(), bytes.end(),
                        [](char byte) { return static_cast<unsigned char>(byte) < 0x80; })) {
            name = QLatin1StringView(bytes.data(), bytes.size());
        } else {
            const QByteArray raw = QByteArray::fromRawData(bytes.data(), bytes.size());
            if (!nameFromBytes(raw, &decoded)) {
                if (unnameable)
                    unnameable->append(escapedBytes(raw));
                continue;
            }
            name = QStringView(decoded);
        }
        if (!visitor(name, entry->d_type)) {
            ::closedir(handle);
            return true;
        }
    }
    // Kept before `closedir`, which has an errno of its own to set.
    const int failure = errno;
    ::closedir(handle);
    if (failure != 0) {
        if (error)
            *error = systemSaid(directory, failure);
        if (state)
            *state = stateForErrno(failure, directory);
        return false;
    }
    return true;
}

} // namespace

bool visitDirectory(const QString &directory,
                    const std::function<bool(QAnyStringView, unsigned char)> &visitor,
                    QStringList *unnameable, QString *error, PathState *state)
{
    return visitEntries(::opendir(QFile::encodeName(directory).constData()), directory,
                        visitor, unnameable, error, state);
}

bool visitDirectory(int descriptor, const QString &directory,
                    const std::function<bool(QAnyStringView, unsigned char)> &visitor,
                    QStringList *unnameable, QString *error, PathState *state)
{
    const int copy = ::dup(descriptor);
    DIR *handle = copy < 0 ? nullptr : ::fdopendir(copy);
    if (!handle && copy >= 0) {
        const int failure = errno;
        ::close(copy);
        errno = failure;
    }
    return visitEntries(handle, directory, visitor, unnameable, error, state);
}

int openDirectoryNoFollow(const QString &path)
{
    if (!isCleanAbsolute(path)) {
        errno = EINVAL;
        return -1;
    }
    int parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const auto close = qScopeGuard([&] { if (parent >= 0) ::close(parent); });
    if (parent < 0)
        return -1;
    const auto parts = path.split(u'/', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int child = ::openat(parent, QFile::encodeName(part).constData(),
                                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0)
            return -1;
        ::close(parent);
        parent = child;
    }
    const int result = parent;
    parent = -1;
    return result;
}

QString removeTreeAt(int parent, const QByteArray &name)
{
    struct stat state{};
    if (::fstatat(parent, name.constData(), &state, AT_SYMLINK_NOFOLLOW) != 0)
        return systemReason(errno);
    if (!S_ISDIR(state.st_mode))
        return ::unlinkat(parent, name.constData(), 0) == 0 ? QString() : systemReason(errno);
    const int child = ::openat(parent, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const auto close = qScopeGuard([child] { if (child >= 0) ::close(child); });
    if (child < 0)
        return systemReason(errno);
    const int copy = ::dup(child);
    DIR *stream = copy < 0 ? nullptr : ::fdopendir(copy);
    if (!stream) {
        const int failure = errno;
        if (copy >= 0)
            ::close(copy);
        return systemReason(failure);
    }
    QString error;
    errno = 0;
    while (const auto *entry = ::readdir(stream)) {
        const QByteArray bytes(entry->d_name);
        if (bytes == "." || bytes == "..")
            continue;
        error = removeTreeAt(child, bytes);
        if (!error.isEmpty())
            break;
        errno = 0;
    }
    const int failure = errno;
    ::closedir(stream);
    if (!error.isEmpty())
        return error;
    if (failure)
        return systemReason(failure);
    struct stat current{};
    if (::fstatat(parent, name.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0)
        return systemReason(errno);
    if (current.st_dev != state.st_dev || current.st_ino != state.st_ino)
        return QStringLiteral("directory changed during deletion");
    return ::unlinkat(parent, name.constData(), AT_REMOVEDIR) == 0 ? QString() : systemReason(errno);
}

} // namespace omafiles
