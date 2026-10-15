#pragma once

#include <QFile>
#include <QAnyStringView>
#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <functional>

class QUrl;

struct stat;

namespace omafiles {

/// The XDG base directories: the variable, or its default under `$HOME` where
/// it is unset or empty.
QString dataHome();
QString configHome();
QString cacheHome();
QString stateHome();
/// False, naming the variable, where the bytes deciding one of the four above
/// are not valid UTF-8, or `HOME` decides one and is not absolute. A binary
/// asks this before it reads or writes a file.
bool environmentNameable(QString *error);
/// False, with the reason, where the current directory is gone or its bytes are
/// not valid UTF-8: a relative path is found through it, and Qt spells it ""
/// or with U+FFFD.
bool workingDirectoryNameable(QString *error);

QString defaultIndexPath();
/// `$HOME` as text, or `/` where it is unset, relative or not valid UTF-8, the
/// root `QDir::homePath` falls back to.
QString homeDirectory();
/// `path` made absolute against the current directory, nothing resolved and
/// nothing cleaned; empty where the current directory cannot be named.
QString anchoredPath(const QString &path);
/// `path` with its deepest ancestor that resolves resolved by the kernel and the
/// rest as typed, cut to its longest leading run of whole components that is
/// text; empty where the working directory is gone.
QString physicalPath(const QString &path);
/// The path a `file:` URL names, decoded to bytes first: empty for a URL that
/// is not a local file, one holding a query or a fragment, which name another
/// file, an escaped `/`, as GLib's `g_filename_from_uri` refuses one, and bytes
/// that are not valid UTF-8.
QString localFileOf(const QUrl &url);

/// `bytes` as text, where the text encodes back to exactly those bytes. False
/// for bytes that are not valid UTF-8, or that hold a NUL: either way the text
/// would name some other file.
bool nameFromBytes(const QByteArray &bytes, QString *name);
/// The words after the program's name as text, or false with `error` naming the
/// first that `nameFromBytes` refuses, which a caller must not act on.
bool argumentsFromBytes(int argc, char **argv, QStringList *words, QString *error);
/// `bytes` with a UTF-8 byte order mark at the start taken off, the way
/// Python's `utf-8-sig` reads a file. RFC 8259 8.1 lets a JSON reader ignore one.
QByteArray withoutBom(const QByteArray &bytes);
/// What the system says `errno` value `failure` means, in the locale's words.
QString systemReason(int failure);
/// A word typed or read, for a message, on one line: as `escapedWords` spells
/// it, and between single quotes where it is empty or has a space at an end,
/// as coreutils `ls ''` says one.
QString spelled(const QString &word);
/// `bytes` as UTF-8, by PEP 383's surrogateescape: a byte that does not decode
/// is U+DC00 plus the byte, so the text keeps every byte until it is spelled. A
/// NUL is U+0000.
QString surrogateEscaped(const QByteArray &bytes);
/// Text nobody here chose, on one line whatever it holds: a tab `\t`, a newline
/// `\n`, every other control and every byte `surrogateEscaped` kept `\xHH`, a
/// C1 control as its UTF-8, and a backslash `\\` so none can be forged.
/// `bytesOnly` spells the kept bytes and NUL alone, for JSON, which carries
/// every character as it is.
QString escapedWords(const QString &text, bool bytesOnly = false);
/// `bytes` for a diagnostic, as `escapedWords` spells them.
QString escapedBytes(const QByteArray &bytes);
/// What the system says about a path: it is there, it is gone, it is under a
/// root nobody has plugged in, or the system will not say.
///
/// A caller acts on Missing and steps over the other two, so `prune` cannot
/// drop a row for a file it merely was not allowed to look at -- nor for a file
/// on a disk that is sitting in a drawer. Unreachable is the whole reason
/// declared roots exist: without them an unmounted archive reads as `ENOENT`,
/// which is indistinguishable from every one of its files having been deleted.
enum class PathState { Present, Missing, Unknown, Unreachable };
///
/// A path under a declared root that is away is Unreachable even where it
/// stats: the declaration says the storage is not here, so whatever answers at
/// that path is not it -- an unmounted mount point with stray files in it. The
/// root is looked up where the path is typed, so one through a root whose own
/// directory is now a symlink is under it, and then where the kernel resolves
/// it, so a relative one or one through a symlink finds it too.
PathState pathState(const QString &path, QString *error);
/// The half of `pathState` that asks the roots, of a path resolved already: Present
/// where no declared root holds it or its root is here, Unreachable where the
/// root is away, Unknown where the root cannot be checked, each with the reason.
PathState storageState(const QString &path, QString *error);
/// `pathState` for an index row, which is Present only where `realpath` gives the
/// row back. A symlink at the row or above it, ENOTDIR and ELOOP are Missing:
/// none is the file the row named, so `prune` may drop the row and `verify`
/// calls it missing.
PathState rowState(const QString &path, QString *error);
/// What `storedSpelling` learnt of each directory, for a caller asking of many
/// paths: one that does not fold case costs a single look, not one per name.
struct Spellings {
    struct Dir {
        QString stored;
        std::optional<bool> folds;
        QHash<QString, QStringList> held;  // where it folds: its names by `foldedName`
    };
    QHash<QString, Dir> dirs;  // by the spelling given
};
/// `name` in NFC and case-folded: two names a folding directory may take for one.
QString foldedName(const QString &name);
/// Every component of an absolute path as its directory stores it, where a
/// directory that folds case reaches the same file by another spelling or an
/// alias; the rest as given.
QString storedSpelling(const QString &path, Spellings *known = nullptr);
/// Whether `dir` folds case, as vfat, exfat and casefold directories do, told
/// by an entry it holds. No for one holding no name with a letter in it.
bool foldsCase(const QString &dir);
/// Whether following `path` ends at no name: `stat` says ENOENT or ENOTDIR,
/// the two findutils reads as a dangling link. A loop or a shut door is not.
bool leadsNowhere(const QString &path);
/// What `stat` alone says, or `lstat` where `followLinks` is false: Present,
/// Missing for ENOENT and nothing else, Unknown with the system's reason for
/// every other failure, and what it read in `found` where that is not null. No
/// root is consulted.
PathState statState(const QString &path, QString *error, bool followLinks = true,
                    struct stat *found = nullptr);
/// What `openToRead` opens: a file this program keeps, which is a regular file
/// or refused and is opened without blocking, or a stream somebody named.
enum class Reading { Kept, Stream };
/// Opens `file`, named already, to read, on the rule `statState` draws: Present
/// when it opened, Missing only for ENOENT, Unknown with the reason otherwise.
/// An opened `file` no longer knows its name.
PathState openToRead(QFile *file, QIODevice::OpenMode mode, QString *error,
                     Reading kind = Reading::Kept);
/// `mkdir -p`, with the system's reason where a component would not be made.
bool makePath(const QString &directory, QString *error);
/// The bytes a symlink holds, exactly as written, or empty on failure. No link
/// holds an empty target, so empty is unambiguous.
QByteArray linkTarget(const QByteArray &path);
/// `realpath(3)` of `path` as text, asking no root: the name, or empty with the
/// state and the reason. A path the kernel resolves to bytes that are not
/// valid UTF-8 is Unknown, with `unnameable` set.
QString realPath(const QString &path, QString *error, PathState *state = nullptr,
                 bool *unnameable = nullptr);
/// The path a caller names, resolved: `realPath` and then the root it is typed
/// under or resolves under, so a path under a root that is away is Unreachable
/// whatever answers at it.
///
/// `state` is which reason it was, for the caller that has to tell a path that
/// is not there from one the system would not describe.
QString canonicalPath(const QString &path, QString *error, PathState *state = nullptr);
/// Why `path`, which `realpath` answered ENOENT for, is not there, where the
/// first component that does not resolve is a symlink: every hop of it, each
/// where the kernel would look for it, so the one to create or repoint is the
/// last one named. Empty where no symlink is on the way.
QString danglingChain(const QString &path);
/// The named path, with everything but its last component resolved.
///
/// POSIX `unlink()` acts on the link, not its target, and `rm`, `mv` and `gio
/// trash` follow suit. `canonicalPath` resolves the last component too, so the
/// verbs that work on the path itself use this one.
QString canonicalLinkPath(const QString &path, QString *error, PathState *state = nullptr);
/// The directory `path` names its last component in, as typed -- `.` for a bare
/// name -- with that component in `name`. Nothing is cleaned: `L/..` is the
/// kernel's to resolve, and after a symlink it is not `.`.
QString typedDirectory(const QString &path, QString *name);
/// A path relative to something, in its clean form and with no `..` climbing
/// out of it -- the shape of a row under a root, a line of a list of names and a
/// root's check.
bool isCleanRelative(const QString &path);
/// An absolute path in its clean form -- the shape of a declared root, a
/// view's dir and a tree a sweep receipt names.
bool isCleanAbsolute(const QString &path);
/// `path` cleaned as text where that cannot change the file it names, empty
/// where it has a `..`: the kernel resolves a symlink before the `..` after it.
QString cleanedAsText(const QString &path);
/// `text` as one shell word to paste, on one line: as it is where it holds
/// nothing a shell reads, in single quotes otherwise, as git's `sq_quote` spells
/// one, with a control character as `$'\xHH'`, as coreutils' shell-escape does.
QString shellQuoted(const QString &text);
bool isUnderDirectory(const QString &path, const QString &directory);
/// The rest of `path` below `directory`, which holds it (`isUnderDirectory`):
/// `.` where the two are the same.
QString relativeTo(const QString &path, const QString &directory);
/// Every name one directory holds, with the ones no path here can spell kept
/// apart.
///
/// A filename is bytes and a path here is text: the index is UTF-8 by
/// definition and so is an xattr value, so a name that is not valid UTF-8
/// belongs to neither, and Qt's own directory listing drops it before a caller
/// ever sees it. `unnameable` collects those, spelled by `escapedBytes`;
/// `names` is everything else, `.` and `..` out. Either list may be null.
///
/// False with `error` filled when the directory would not open or a read of it
/// stopped part way, and `state` tells a directory that is not there from one
/// the system declines to describe -- the distinction `pathState` draws, on the
/// same rule. False means the lists are short by an unknown amount, so a caller
/// answers with the failure and not with what it collected.
bool readDirectory(const QString &directory, QStringList *names, QStringList *unnameable,
                   QString *error, PathState *state = nullptr);
/// Visits nameable entries with their dirent type, stopping successfully when
/// `visitor` returns false. Error and unnameable-name handling match readDirectory.
/// Names live only during the callback: ASCII borrows Latin1 bytes, Unicode is UTF16.
bool visitDirectory(const QString &directory,
                    const std::function<bool(QAnyStringView, unsigned char)> &visitor,
                    QStringList *unnameable, QString *error, PathState *state = nullptr);


/// Visits a borrowed directory descriptor, using `directory` only in diagnostics.
bool visitDirectory(int descriptor, const QString &directory,
                    const std::function<bool(QAnyStringView, unsigned char)> &visitor,
                    QStringList *unnameable, QString *error, PathState *state = nullptr);

// Borrowed descriptor removal and anchored directory traversal never follow links.
int openDirectoryNoFollow(const QString &path);
QString removeTreeAt(int parent, const QByteArray &name);

} // namespace omafiles
