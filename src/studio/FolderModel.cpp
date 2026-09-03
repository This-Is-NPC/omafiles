#include "FolderModel.h"

#include "IndexModel.h"
#include "Paths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QMimeDatabase>
#include <QMimeType>
#include <QSet>
#include <algorithm>

namespace omafiles {
namespace {

QString formatSize(qint64 bytes)
{
    if (bytes < 0)
        return {};
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    qreal value = static_cast<qreal>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QString::number(bytes) + QStringLiteral(" B");
    return QString::number(value, 'f', value >= 10.0 ? 0 : 1) + QLatin1Char(' ')
        + QString::fromLatin1(units[unit]);
}

QString formatMtime(qint64 secs)
{
    if (secs <= 0)
        return {};
    return QLocale::system().toString(QDateTime::fromSecsSinceEpoch(secs),
                                       QStringLiteral("yyyy-MM-dd HH:mm"));
}


// What a row is, in one short word: the suffix, not the mime description. The
// mime database has no entry for `.drawio`, so a Type column of descriptions
// reads "Plain text document" against every diagram here -- and a suffix is six
// characters where a description is twenty-four.
//
// Only a file with no suffix falls back to the mime, and then only to its top
// half: `text`, `image`, `video`. That is the most that can be said about a
// file called `README`.
QString typeOf(const QFileInfo &info)
{
    if (info.isDir())
        return QStringLiteral("folder");
    // A name the system will not resolve has no suffix worth reading: a symlink
    // that leads nowhere -- or round to itself -- is a link and nothing else,
    // and a fifo, a socket or a device is not a document whatever it is called.
    // The details panel carries the exact mime for the row under the cursor.
    if (info.isSymLink() && !info.exists())
        return QStringLiteral("broken link");
    if (!info.isFile())
        return QStringLiteral("special");
    const QString suffix = info.suffix().toLower();
    if (!suffix.isEmpty())
        return suffix;

    static QMimeDatabase mime;
    const QString name = mime.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
    return name.section(QLatin1Char('/'), 0, 0);
}

} // namespace

FolderModel::FolderModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_directory(QDir::homePath())
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(60);
    connect(&m_settle, &QTimer::timeout, this, &FolderModel::rebuild);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { m_settle.start(); });
}

// The watch follows the folder on screen and nothing else, and is re-taken on
// every read: a directory replaced or removed takes its watch with it, the same
// way a renamed file does. A tag listing has no one directory to be about, so it
// watches nothing and `IndexModel::changed` is what reloads it.
void FolderModel::watch()
{
    const QStringList wanted = m_selectedTag.isEmpty() ? QStringList{m_directory}
                                                       : QStringList{};
    if (m_watcher.directories() == wanted)
        return;
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!wanted.isEmpty())
        m_watcher.addPaths(wanted);
}

void FolderModel::setTab(TabState *tab)
{
    if (m_tab == tab)
        return;
    if (m_tab)
        disconnect(m_tab, nullptr, this, nullptr);
    m_tab = tab;
    if (m_tab) {
        connect(m_tab, &TabState::markedChanged, this, &FolderModel::marksChanged);
        // A closed tab is deleted while the binding that points here still
        // names it.
        connect(m_tab, &QObject::destroyed, this, [this] { m_tab = nullptr; });
    }
    emit tabChanged();
    // A rebuild rather than a repaint: the cursor and the marks both come off
    // the new tab, and two tabs standing in the same folder change neither the
    // directory nor the query to say so.
    rebuild();
}

void FolderModel::marksChanged()
{
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {MarkedRole});
    emit markedChanged();
}

int FolderModel::markedCount() const
{
    return m_tab ? m_tab->markedCount() : 0;
}

void FolderModel::setSource(IndexModel *index)
{
    if (m_index == index)
        return;
    if (m_index)
        disconnect(m_index, nullptr, this, nullptr);
    m_index = index;
    if (m_index)
        connect(m_index, &IndexModel::changed, this, &FolderModel::reload);
    emit sourceChanged();
    rebuild();
}

void FolderModel::setDirectory(const QString &path)
{
    const QFileInfo info(path.isEmpty() ? QDir::homePath() : path);
    const QString next = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
    if (m_directory == next)
        return;
    m_directory = next;
    emit directoryChanged();
    rebuild();
}

void FolderModel::setFilter(const QString &text)
{
    if (m_filter == text)
        return;
    m_filter = text;
    emit queryChanged();
    rebuild();
}

void FolderModel::setSelectedTag(const QString &tag)
{
    if (m_selectedTag == tag)
        return;
    m_selectedTag = tag;
    emit selectedTagChanged();
    rebuild();
}

void FolderModel::setShowHidden(bool show)
{
    if (m_showHidden == show)
        return;
    m_showHidden = show;
    emit queryChanged();
    rebuild();
}

void FolderModel::setSortRole(const QString &role)
{
    const QString next = TabState::validSortRole(role);
    if (m_sortRole == next)
        return;
    m_sortRole = next;
    emit sortChanged();
    rebuild();
}

void FolderModel::setSortAscending(bool ascending)
{
    if (m_sortAscending == ascending)
        return;
    m_sortAscending = ascending;
    emit sortChanged();
    rebuild();
}

QString FolderModel::parentDirectory() const
{
    const QDir dir(m_directory);
    if (dir.isRoot())
        return m_directory;
    return QFileInfo(m_directory).absolutePath();
}

// A tag listing is not a place in the tree, so there is nothing above it: the
// files on it live in as many folders as they like, and no single one of them is
// the one you would walk up into.
bool FolderModel::atRoot() const
{
    return !m_selectedTag.isEmpty() || QDir(m_directory).isRoot();
}

int FolderModel::taggedCount() const
{
    int n = 0;
    for (const Entry &entry : m_rows) {
        if (entry.tagged)
            ++n;
    }
    return n;
}

bool FolderModel::validRow(int row) const
{
    return row >= 0 && row < m_rows.size();
}

int FolderModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QVariant FolderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Entry &row = m_rows.at(index.row());
    switch (role) {
    case PathRole:
        return row.path;
    case NameRole:
    case Qt::DisplayRole:
        return row.name;
    case KindRole:
        return row.kind;
    case TypeRole:
        return row.type;
    case TagsRole:
        return row.tags;
    case MarkedRole:
        return m_tab && m_tab->isMarked(row.path);
    case SizeTextRole:
        return row.sizeText;
    case MtimeTextRole:
        return row.mtimeText;
    default:
        return {};
    }
}

QHash<int, QByteArray> FolderModel::roleNames() const
{
    return {{PathRole, "path"},
            {NameRole, "name"},
            {KindRole, "kind"},
            {TypeRole, "type"},
            {TagsRole, "tags"},
            {MarkedRole, "marked"},
            {SizeTextRole, "sizeText"},
            {MtimeTextRole, "mtimeText"}};
}

// Asking for a folder is asking to stop looking at a tag: both of these land on
// a directory listing, or the key would appear to do nothing.
void FolderModel::goHome()
{
    setSelectedTag(QString());
    setDirectory(QDir::homePath());
}

void FolderModel::goUp()
{
    if (atRoot())
        return;
    // Walking out of a folder should land the cursor on that folder, not on the
    // top of its parent: going up and coming back down has to be symmetric or
    // the keyboard loses its place every time.
    if (m_tab)
        m_tab->setCursorPath(m_directory);
    setDirectory(parentDirectory());
}

void FolderModel::enter(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir())
        return;
    if (m_tab)
        m_tab->setCursorPath(QString());
    setSelectedTag(QString());
    setDirectory(path);
}

void FolderModel::activate(const QString &path, const QString &kind)
{
    // The `..` row is the same move as the up key, not a folder that happens to
    // be the parent: taking it through `enter()` would forget which folder you
    // came out of, so the two ways of going up would land in different places.
    if (kind == QLatin1String("parent")) {
        goUp();
    } else if (kind == QLatin1String("dir")) {
        enter(path);
    } else if (m_index) {
        // Only a regular file goes to the desktop's opener. A fifo hands it a
        // read that may never return, and a socket or a device is not a
        // document. A link that leads nowhere does go through, because
        // `openPath` is what names both ends of it.
        const QFileInfo info(path);
        if (info.exists() && !info.isFile()) {
            m_error = QStringLiteral("%1 is not a regular file; nothing was opened").arg(path);
            emit changed();
            return;
        }
        m_index->openPath(path);
    }
}

void FolderModel::activateRow(int row)
{
    if (!validRow(row))
        return;
    const Entry &entry = m_rows.at(row);
    activate(entry.path, entry.kind);
}

void FolderModel::toggleMarkAt(int row)
{
    if (!validRow(row) || !m_tab)
        return;
    const Entry &entry = m_rows.at(row);
    if (entry.kind == QLatin1String("parent"))
        return;
    m_tab->toggleMark(entry.path);
}

void FolderModel::markAll()
{
    if (!m_tab)
        return;
    QSet<QString> marks;
    for (const Entry &entry : std::as_const(m_rows)) {
        if (entry.kind != QLatin1String("parent"))
            marks.insert(entry.path);
    }
    m_tab->setMarked(marks);
}

void FolderModel::clearMarks()
{
    if (m_tab)
        m_tab->setMarked({});
}

QString FolderModel::pathAt(int row) const
{
    return validRow(row) ? m_rows.at(row).path : QString();
}

QString FolderModel::kindAt(int row) const
{
    return validRow(row) ? m_rows.at(row).kind : QString();
}

QVariantMap FolderModel::entryAt(int row) const
{
    if (!validRow(row))
        return {};
    const Entry &entry = m_rows.at(row);
    const bool directory = entry.kind != QLatin1String("file");

    // MatchDefault, not MatchExtension: this runs for the focused row only, so
    // it can afford to look inside the file when the name does not say. By
    // extension alone `.drawio` is `application/octet-stream`, which tells the
    // reader less than nothing; a glance at the first bytes makes it
    // `text/plain`.
    static QMimeDatabase mime;
    const QString type = directory
        ? QStringLiteral("inode/directory")
        : mime.mimeTypeForFile(entry.path, QMimeDatabase::MatchDefault).name();

    // Counted here and nowhere else: it costs an open() per directory, so it is
    // paid for the one row under the cursor rather than for every row on every
    // rebuild. On a big tree, or a network mount, the difference is the list
    // being usable.
    //
    // Every name the folder holds, hidden ones and the ones no path here can
    // spell included, read through the one reader the listing uses -- a count
    // that filters on what a stat says each entry is calls a folder of dangling
    // symlinks and fifos empty. -1 where the folder would not be read at all.
    int items = -1;
    if (directory && entry.kind != QLatin1String("parent")) {
        QStringList names;
        QStringList unnameable;
        if (readDirectory(entry.path, &names, &unnameable, nullptr))
            items = names.size() + unnameable.size();
    }

    return {{QStringLiteral("path"), entry.path},
            {QStringLiteral("items"), items},
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("kind"), entry.kind},
            {QStringLiteral("type"), entry.type},
            {QStringLiteral("directory"), directory},
            {QStringLiteral("mime"), type},
            {QStringLiteral("size"), entry.size},
            {QStringLiteral("sizeText"), entry.sizeText},
            {QStringLiteral("mtime"), entry.mtime},
            {QStringLiteral("mtimeText"), entry.mtimeText},
            {QStringLiteral("marked"), m_tab && m_tab->isMarked(entry.path)},
            {QStringLiteral("tagged"), entry.tagged},
            {QStringLiteral("tags"), entry.tags.isEmpty()
                                          ? QStringList()
                                          : entry.tags.split(QLatin1Char(' '))}};
}

int FolderModel::rowForPath(const QString &path) const
{
    if (path.isEmpty())
        return -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).path == path)
            return i;
    }
    return -1;
}

void FolderModel::rememberCursor(int row)
{
    if (m_tab)
        m_tab->setCursorPath(validRow(row) ? m_rows.at(row).path : QString());
}

int FolderModel::cursorRow() const
{
    if (m_rows.isEmpty())
        return -1;
    const int row = m_tab ? rowForPath(m_tab->cursorPath()) : -1;
    return row >= 0 ? row : 0;
}

void FolderModel::reload()
{
    rebuild();
}

void FolderModel::applySort(QVector<Entry> *rows) const
{
    std::sort(rows->begin(), rows->end(), [this](const Entry &a, const Entry &b) {
        int cmp = 0;
        if (m_sortRole == QLatin1String("size")) {
            if (a.size != b.size)
                cmp = a.size < b.size ? -1 : 1;
        } else if (m_sortRole == QLatin1String("mtime")) {
            if (a.mtime != b.mtime)
                cmp = a.mtime < b.mtime ? -1 : 1;
        } else if (m_sortRole == QLatin1String("type")) {
            cmp = QString::compare(a.type, b.type, Qt::CaseInsensitive);
        } else if (m_sortRole == QLatin1String("tags")) {
            // By the first tag, with the untagged after everything -- which is
            // the question this program exists to answer, and which reversing
            // the sort turns into "show me the untagged first".
            const QString first = a.tags.section(QLatin1Char(' '), 0, 0);
            const QString other = b.tags.section(QLatin1Char(' '), 0, 0);
            if (first.isEmpty() != other.isEmpty())
                cmp = first.isEmpty() ? 1 : -1;
            else
                cmp = QString::compare(first, other, Qt::CaseInsensitive);
        }
        if (cmp == 0)
            cmp = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return m_sortAscending ? cmp < 0 : cmp > 0;
    });
}

FolderModel::Entry FolderModel::describe(const QFileInfo &info, const QString &name) const
{
    Entry row;
    row.path = info.canonicalFilePath().isEmpty() ? info.absoluteFilePath()
                                                  : info.canonicalFilePath();
    row.name = name;
    row.type = typeOf(info);
    row.size = info.isFile() ? info.size() : 0;
    row.mtime = info.lastModified().toSecsSinceEpoch();
    row.sizeText = info.isFile() ? formatSize(row.size) : QString();
    row.mtimeText = formatMtime(row.mtime);
    // Folders carry tags too -- `omafiles tag add -- some-dir` works -- so the
    // lookup below runs for them as well, or a tagged folder shows no chips here
    // and counts for nothing.
    row.kind = info.isDir() ? QStringLiteral("dir") : QStringLiteral("file");
    if (m_index) {
        const QStringList tags = m_index->tagsForPath(row.path);
        row.tags = tags.join(QLatin1Char(' '));
        row.tagged = !tags.isEmpty();
    }
    return row;
}

// Every file in the index carrying the tag, wherever in the tree it is: the set
// `omafiles ls --tag` answers with, so the window and the command line cannot
// disagree about who carries a tag. A row whose file is gone is dropped; one the
// system merely declines to describe is named, the way `ls` warns about it.
//
// The hidden-files switch gets no say: a dotfile you tagged by name is an answer
// to that tag, and dropping it would make the list disagree with `ls` over a
// preference about folders.
void FolderModel::listTag()
{
    if (!m_index)
        return;

    const QString needle = m_filter.trimmed().toLower();
    QStringList declined;
    QVector<Entry> rows;
    for (const Record &record : m_index->records()) {
        if (!record.tags.contains(m_selectedTag))
            continue;
        QString reason;
        const PathState state = pathState(record.path, &reason);
        if (state == PathState::Unknown) {
            declined.append(reason);
            continue;
        }
        if (state == PathState::Missing)
            continue;
        const QFileInfo info(record.path);
        if (!needle.isEmpty() && !info.fileName().toLower().contains(needle))
            continue;
        rows.append(describe(info, info.fileName()));
    }

    if (!declined.isEmpty()) {
        m_error = QStringLiteral("%1 row(s) not listed; %2")
                      .arg(declined.size())
                      .arg(declined.first());
    }
    applySort(&rows);
    m_rows = rows;
}

void FolderModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    m_error.clear();
    if (m_selectedTag.isEmpty())
        listDirectory();
    else
        listTag();
    watch();
    endResetModel();
    emit changed();
}

void FolderModel::listDirectory()
{
    const QDir dir(m_directory);
    const QString needle = m_filter.trimmed().toLower();

    // Before the directory is read rather than after: a folder that is not
    // there is still a folder to walk out of, and `..` is the only row that
    // does it.
    if (!atRoot()) {
        Entry parent;
        parent.path = parentDirectory();
        parent.name = QStringLiteral("..");
        parent.kind = QStringLiteral("parent");
        m_rows.append(parent);
    }

    // Read as names rather than through `QDir::entryInfoList`, which filters on
    // what a stat says each one is and so drops a dangling symlink, a symlink
    // round to itself and a fifo from a folder that holds them. A name is a
    // name; what it turns out to be is the row's business below.
    QStringList names;
    QStringList unnameable;
    QString reason;
    PathState state = PathState::Present;
    if (!readDirectory(m_directory, &names, &unnameable, &reason, &state)) {
        m_error = state == PathState::Missing
            ? QStringLiteral("path missing: %1").arg(m_directory)
            : QStringLiteral("%1; nothing in it was listed").arg(reason);
        return;
    }
    // Counted and named, not stepped over: a list one row short that looks
    // complete is the failure this says out loud.
    if (!unnameable.isEmpty())
        m_error = QStringLiteral("not listed, the name is not valid UTF-8: %1")
                      .arg(unnameable.join(QStringLiteral(", ")));

    QVector<Entry> dirs;
    QVector<Entry> files;
    for (const QString &name : std::as_const(names)) {
        if (name.startsWith(QLatin1Char('.')) && !m_showHidden)
            continue;
        if (!needle.isEmpty() && !name.toLower().contains(needle))
            continue;

        const QFileInfo info(dir.filePath(name));
        const Entry row = describe(info, name);
        if (info.isDir())
            dirs.append(row);
        else
            files.append(row);
    }

    applySort(&dirs);
    applySort(&files);
    m_rows += dirs;
    m_rows += files;
}

} // namespace omafiles
