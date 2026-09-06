#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace omafiles {

/// One saved way of asking, kept by name.
///
/// A folder, a tag and a query are the same thing here, which is why there is
/// one of these and not three. A folder is a question about a place, a tag is a
/// question with one term in it, and what the roadmap calls a saved search is
/// the same question with more terms -- so pinning any of them is saving a view,
/// and the panel that lists them does not have to know which kind it is looking
/// at until somebody opens one.
///
/// Either a `dir` or `tags`, never both. Scoping a tag query to a folder is a
/// question the window cannot walk yet -- a tags tab is not standing anywhere --
/// and a view that means something at a prompt and something else in a panel is
/// worse than one that is refused.
struct View {
    QString name;
    QString dir;
    QStringList tags;

    bool isFolder() const { return tags.isEmpty(); }
};

/// Where they live: one JSON object per line, the shape the index and the roots
/// use, so the same eyes and the same tools read all three.
QString defaultViewsPath();

bool loadViews(const QString &path, QVector<View> *views, QString *error);
bool loadViews(QVector<View> *views, QString *error);
bool saveViews(const QString &path, const QVector<View> &views, QString *error);

/// `[a-z0-9-]` and spaces, trimmed, at most 64. A name is typed at a prompt and
/// shown in a panel, so it may hold a space where a tag may not; it may not hold
/// the newline or the tab that would make the file unreadable a line at a time.
bool isValidViewName(const QString &name);
QString normalizeViewName(const QString &raw, QString *error);

} // namespace omafiles
