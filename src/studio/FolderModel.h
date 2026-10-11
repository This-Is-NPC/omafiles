#pragma once

#include "IndexModel.h"
#include "Workspace.h"

#include <QAbstractListModel>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QtQmlIntegration/qqmlintegration.h>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

namespace omafiles {

class FolderModel : public QAbstractListModel
{
    Q_OBJECT
    /// Declared, not registered in code -- see the note on `GraphItem`.
    QML_ELEMENT
    Q_PROPERTY(omafiles::IndexModel *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(omafiles::TabState *tab READ tab WRITE setTab NOTIFY tabChanged)
    /// The tab's `cwd`, which is the one place a listing's folder is set.
    Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY queryChanged)
    /// See `TabState::criteria`.
    Q_PROPERTY(QVariantMap criteria READ criteria WRITE setCriteria NOTIFY queryChanged)
    Q_PROPERTY(QString selectedTag READ selectedTag WRITE setSelectedTag NOTIFY selectedTagChanged)
    Q_PROPERTY(QStringList facets READ facets WRITE setFacets NOTIFY facetsChanged)
    Q_PROPERTY(bool untagged READ untagged WRITE setUntagged NOTIFY facetsChanged)
    /// A shell pattern answered across the roots, out of the lists of names;
    /// see `TabState::pattern`.
    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY patternChanged)
    /// What the desktop opened lately, instead of a folder; see
    /// `TabState::recent`.
    Q_PROPERTY(bool recent READ recent WRITE setRecent NOTIFY recentChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY queryChanged)
    Q_PROPERTY(bool atRoot READ atRoot NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int count READ rowCount NOTIFY changed)
    Q_PROPERTY(int markedCount READ markedCount NOTIFY markedChanged)
    Q_PROPERTY(int taggedCount READ taggedCount NOTIFY changed)
    Q_PROPERTY(QString sortRole READ sortRole WRITE setSortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending WRITE setSortAscending NOTIFY sortChanged)

public:
    enum Roles {
        PathRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        TypeRole,
        TagsRole,
        MarkedRole,
        SizeTextRole,
        MtimeTextRole,
        /// How many files a `tag` row leads to, and -1 on every other row. A
        /// facet without a count is a folder you have to open to find out
        /// whether it was worth opening.
        CountRole,
        OwnerRole,
        /// Permissions as `ls -l` spells them, `drwxr-xr-x`.
        ModeRole
    };

    explicit FolderModel(QObject *parent = nullptr);

    IndexModel *source() const { return m_index; }
    void setSource(IndexModel *index);

    /// The tab this listing belongs to, and the only place its marks and its
    /// cursor are kept. Two tabs in one pane are drawn by one of these models,
    /// so anything remembered here instead would follow the view across a
    /// switch and describe the wrong folder.
    TabState *tab() const { return m_tab; }
    void setTab(TabState *tab);

    QString directory() const;

    QString filter() const { return m_filter; }
    void setFilter(const QString &text);
    QVariantMap criteria() const { return m_criteria; }
    void setCriteria(const QVariantMap &criteria);

    /// The tags this listing has been narrowed by, in a `tags` tab.
    ///
    /// Not a place in the tree and not one tag either: the rows are the files
    /// carrying *all* of them, and the folders above them are the tags that
    /// still tell those files apart. `#work/#draft` is a question, so its
    /// answer does not change when it is asked the other way round.
    QStringList facets() const { return m_facets; }
    void setFacets(const QStringList &facets);
    bool untagged() const { return m_untagged; }
    void setUntagged(bool untagged);

    /// The tag this listing is showing, or empty for a directory listing.
    ///
    /// A tag names files wherever they are, so a tag listing is not a folder:
    /// its rows are the index rows carrying the tag, the set `omafiles ls
    /// --tag` answers with. `directory` is left where it was, so letting the
    /// tag go comes back to the folder you picked it from.
    QString selectedTag() const { return m_selectedTag; }
    void setSelectedTag(const QString &tag);
    QString pattern() const { return m_pattern; }
    void setPattern(const QString &pattern);
    bool recent() const { return m_recent; }
    void setRecent(bool recent);

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);

    QString sortRole() const { return m_sortRole; }
    void setSortRole(const QString &role);

    bool sortAscending() const { return m_sortAscending; }
    void setSortAscending(bool ascending);

    /// What the window could not do about this folder, or empty.
    ///
    /// A folder nobody may open and one that is not there draw the same list an
    /// empty one does, so the difference between the three is said here rather
    /// than left to the reader -- through the channel `IndexModel::error`
    /// already has to the status bar.
    QString error() const { return m_error; }

    QString parentDirectory() const;
    bool atRoot() const;
    int markedCount() const;

    /// How many rows in this folder carry a tag.
    ///
    /// The window already says how many tagged files the whole index holds, and
    /// standing in a folder with none of them, that number reads as a promise
    /// about the list in front of you. This is the one that answers it.
    int taggedCount() const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void goHome();
    Q_INVOKABLE void goUp();
    /// Walks to where `path` is and lands the cursor on it.
    ///
    /// The way out of a tag listing, where the rows come from as many folders
    /// as they like and none of them is the one the listing is standing in.
    Q_INVOKABLE void goTo(const QString &path);
    void enter(const QString &path);
    void activate(const QString &path, const QString &kind);
    Q_INVOKABLE void activateRow(int row);
    Q_INVOKABLE void toggleMarkAt(int row);
    Q_INVOKABLE void clearMarks();
    Q_INVOKABLE void markAll();
    Q_INVOKABLE void invertMarks();
    /// Marks every row sharing a tag, or a type, with the one at `path`.
    ///
    /// The two questions a marked set is usually assembled to ask -- everything
    /// filed like this one, everything shaped like this one -- and both of them
    /// are a scan of rows already in memory rather than anything to type.
    Q_INVOKABLE void markSameTag(const QString &path);
    Q_INVOKABLE void markSameType(const QString &path);
    /// Marks every file row from one row to another, either way round, on top
    /// of what is marked. Shift and a click, the way every list does it.
    Q_INVOKABLE void markRange(int from, int to);
    /// Marks `kept` plus `rows`, or `kept` with `rows` toggled against it.
    /// The rubber band calls it on every move with the marks it started from,
    /// so a row the band lets go of goes back to what it was.
    Q_INVOKABLE void markRows(const QList<int> &rows, const QStringList &kept, bool toggle);
    /// Marks every row whose name matches a shell pattern -- `*.pdf`,
    /// `draft-?.md` -- on top of what is marked, and says how many that was.
    Q_INVOKABLE int markMatching(const QString &pattern);

    /// The marked rows, in the order the listing shows them.
    ///
    /// The order matters and the set does not have one: marks are a QSet, whose
    /// order is a hash of the paths, so a caller handing them to `tag add` would
    /// name the same files in a different order on every run and the receipt
    /// would not be readable twice.
    Q_INVOKABLE QStringList markedPaths() const;
    Q_INVOKABLE bool isMarked(const QString &path) const;

    /// What a command chosen over `path` should act on.
    ///
    /// A marked row means the set; an unmarked one means itself, whatever is
    /// marked elsewhere. This is the rule every file manager with a selection
    /// keeps, and it lives here rather than in each menu entry so that no entry
    /// can be written that forgets it -- which is how a delete of one file ends
    /// up taking forty.
    Q_INVOKABLE QStringList targetPaths(const QString &path) const;

    /// The tags on one row, as the index has them.
    Q_INVOKABLE QStringList tagsFor(const QString &path) const;
    Q_INVOKABLE QString pathAt(int row) const;
    Q_INVOKABLE QString kindAt(int row) const;
    Q_INVOKABLE void reload();

    /// Everything the details panel shows about one row, in one read. Empty map
    /// for a row that is not there, so QML can test it with a single truthiness
    /// check instead of guarding every field.
    Q_INVOKABLE QVariantMap entryAt(int row) const;
    Q_INVOKABLE int rowForPath(const QString &path) const;

    /// The row the cursor belongs on after the list is rebuilt.
    ///
    /// A rebuild happens for a filter keystroke, an index reload and a change of
    /// directory alike, and a cursor that jumps to the top on each of those is a
    /// cursor you have to put back by hand. The tab remembers the path under the
    /// cursor, so it survives a rebuild that keeps the row; and `goUp()`
    /// remembers the directory it is leaving, so walking out of a folder puts
    /// the cursor back on the folder you walked out of rather than on `..`.
    Q_INVOKABLE void rememberCursor(int row);
    Q_INVOKABLE int cursorRow() const;
    /// Where the cursor belongs once the list next shows `path` -- a row that
    /// is not there yet. A rename and a new folder both know the name before
    /// the rebuild that lists it, and without this the rebuild would find the
    /// old name gone and put the cursor at the top.
    Q_INVOKABLE void pointCursorAt(const QString &path);

signals:
    void sourceChanged();
    void tabChanged();
    void directoryChanged();
    void selectedTagChanged();
    void facetsChanged();
    void patternChanged();
    void recentChanged();
    void queryChanged();
    void changed();
    void markedChanged();
    void sortChanged();

private:
    struct Entry {
        QString path;
        QString name;
        QString kind;
        QString type;
        QString tags;
        bool tagged = false;
        /// The name is a symlink; `path` is the link's own.
        bool link = false;
        /// Files under a facet row; -1 where the row is not one.
        int count = -1;
        qint64 size = 0;
        qint64 mtime = 0;           // milliseconds, so a sort orders within a second
        QString sizeText;
        QString mtimeText;
        QString owner;
        QString mode;
    };

    bool validRow(int row) const;
    void rebuild();
    void listDirectory();
    void listTag();
    /// The tags that still divide the files under `m_facets`, and those files.
    void listFacets();
    /// Out of the lists of names `sweep` writes rather than by walking the
    /// disk: the complement of the vocabulary -- what carries no tag at all --
    /// or every name fitting `m_pattern`, whichever the tab is asking.
    void listNames();
    /// What the desktop opened lately, in its order, out of its own list.
    void listRecent();
    /// Whether this listing answers a tag question rather than a folder one.
    /// Read off the tab and not stored, so the two cannot drift apart.
    bool browsingFacets() const;
    /// One row, off whatever the system says about the file. `name` is what the
    /// row is called, which is the directory entry in a folder and the last
    /// component of the path in a tag listing.
    Entry describe(const QFileInfo &info, const QString &name) const;
    void applySort(QVector<Entry> *rows) const;
    /// Whether a row passes every criterion. `..` and the tags read as folders
    /// are ways to move, not files, so they always pass.
    bool admits(const Entry &row) const;
    void watch();
    void marksChanged();

    IndexModel *m_index = nullptr;
    TabState *m_tab = nullptr;
    QString m_filter;
    QVariantMap m_criteria;
    QString m_selectedTag;
    QString m_pattern;
    bool m_recent = false;
    QStringList m_facets;
    bool m_untagged = false;
    bool m_showHidden = false;
    QString m_sortRole = QStringLiteral("name");
    bool m_sortAscending = true;
    QString m_error;
    QVector<Entry> m_rows;
    /// The folder on screen, watched so it stays the folder on disk. One
    /// directory and never a tree: an inotify watch is a per-user resource, and
    /// what a list has to be honest about is the list.
    QFileSystemWatcher m_watcher;
    /// Coalesces the watcher. A single copy or save is several filesystem
    /// events, and the list wants the state after them, once.
    QTimer m_settle;
};

} // namespace omafiles
