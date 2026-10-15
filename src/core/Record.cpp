#include "Record.h"

#include "Paths.h"
#include "Roots.h"
#include "Tag.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonValue>

namespace omafiles {

namespace {

QJsonObject rowJson(const Record &record, const QString &path)
{
    QJsonArray tagArray;
    for (const QString &tag : record.tags)
        tagArray.append(tag);
    QJsonObject object{{QStringLiteral("path"), path}, {QStringLiteral("tags"), tagArray}};
    if (record.tagged > 0)
        object.insert(QStringLiteral("tagged"), record.tagged);
    return object;
}

} // namespace

bool Record::operator==(const Record &other) const
{
    return path == other.path && tags == other.tags && tagged == other.tagged;
}

QJsonObject Record::toJson() const
{
    return rowJson(*this, path);
}

// Written under whichever root holds it now, and not under the one it was read
// from: declaring a root is what migrates the rows beneath it, on the next
// write that changes a row, with no verb to run and nothing to convert by hand. A row under no
// declared root keeps its absolute path, which is what every row on a machine
// that declares nothing stays.
QJsonObject Record::toStoredJson() const
{
    QString rootId;
    QString rest;
    splitByRoot(path, &rootId, &rest);
    if (rootId.isEmpty() && !rootById(root)) {
        // A row whose root is not declared, kept exactly as it was read.
        // Anything else would rewrite it as an absolute path built from a mount
        // point that is not there -- which is how the tags of an archive get
        // filed under a directory that does not exist. A row whose root is
        // declared but does not hold it has moved out, and is absolute.
        rootId = root;
        rest = relative;
    }

    if (rootId.isEmpty())
        return rowJson(*this, path);
    QJsonObject object = rowJson(*this, rest);
    object.insert(QStringLiteral("root"), rootId);
    return object;
}

Record Record::fromJson(const QJsonObject &object, QString *error)
{
    Record record;
    // `root` names a declaration, and one that is not there is a row nothing
    // can place: refused by name, with the declaration that places it.
    const QJsonValue rootValue = object.value(QStringLiteral("root"));
    if (!rootValue.isUndefined() && !rootValue.isNull()) {
        if (!rootValue.isString()) {
            if (error)
                *error = QStringLiteral("index row has a non-string root");
            return {};
        }
        record.root = rootValue.toString();
        if (record.root.isEmpty()) {
            if (error)
                *error = QStringLiteral("index row has an empty root");
            return {};
        }
        if (!rootById(record.root)) {
            if (error)
                *error = QStringLiteral("index row names the root %1, which is not declared "
                                        "here; the rows under it are placed once `omafiles "
                                        "roots add --id %2 <dir>` declares it")
                             .arg(spelled(record.root), shellQuoted(record.root));
            return {};
        }
    }

    // Told apart: "you left it out" and "you wrote the wrong kind of thing"
    // send a reader looking in two different places.
    const QJsonValue pathValue = object.value(QStringLiteral("path"));
    if (pathValue.isUndefined() || pathValue.isNull()) {
        if (error)
            *error = QStringLiteral("index row missing path");
        return {};
    }
    if (!pathValue.isString()) {
        if (error)
            *error = QStringLiteral("index row has a non-string path");
        return {};
    }
    record.path = pathValue.toString();
    if (record.path.isEmpty()) {
        if (error)
            *error = QStringLiteral("index row has an empty path");
        return {};
    }
    // The shape this program writes, and nothing else: a NUL or a lone
    // surrogate names some other file once it is bytes, and a path that is
    // relative, or not in its clean form, is a row no verb ever reaches.
    // Under a root the path is relative to it and does not climb out.
    const QString &typed = record.path;
    if (typed.contains(QChar(0)) || !typed.isValidUtf16()) {
        if (error)
            *error = QStringLiteral("index row has a path holding a NUL or a lone surrogate, "
                                    "which no file name can hold");
        return {};
    }
    const QString clean = cleanedAsText(typed);
    const QString shape = record.root.isEmpty() && !QDir::isAbsolutePath(typed)
        ? QStringLiteral("is not an absolute path")
        : record.root.isEmpty() && QDir::cleanPath(typed) != typed
        ? (clean.isEmpty() ? QStringLiteral("is not in its clean form")
                           : QStringLiteral("is not in its clean form, %1").arg(spelled(clean)))
        : !record.root.isEmpty() && !isCleanRelative(typed)
        ? QStringLiteral("is not a clean path inside root %1").arg(record.root)
        : QString();
    if (!shape.isEmpty()) {
        if (error)
            *error = QStringLiteral("index row path %1 %2").arg(spelled(typed), shape);
        return {};
    }
    // Under a root the path is the rest of the way there, and the absolute one
    // is worked out here so that everything downstream sees the same field it
    // always did.
    if (!record.root.isEmpty()) {
        record.relative = record.path;
        record.path = joinRoot(record.root, record.relative);
    }

    // `tags` has to be a list if it is there at all: `toArray()` alone turns
    // anything else into an empty one, so `"tags":"notalist"` would import
    // cleanly as a row with no tags.
    const QJsonValue tagValue = object.value(QStringLiteral("tags"));
    if (!tagValue.isUndefined() && !tagValue.isArray()) {
        if (error)
            *error = QStringLiteral("index row has a non-list tags field");
        return {};
    }
    const QJsonArray tagArray = tagValue.toArray();
    for (const QJsonValue &value : tagArray) {
        if (!value.isString()) {
            if (error)
                *error = QStringLiteral("index row has a non-string tag");
            return {};
        }
        // As written, not folded: a tag this program wrote is already in the
        // shape, and one that is not is a row `tag rm` could never match.
        QString why;
        const QString folded = normalizeTag(value.toString(), &why);
        if (folded.isEmpty() || folded != value.toString()) {
            if (error)
                *error = why.isEmpty()
                    ? QStringLiteral("index row has a tag not in its folded form: %1")
                          .arg(spelled(value.toString()))
                    : QStringLiteral("index row has an %1").arg(why);
            return {};
        }
        record.tags.append(value.toString());
    }
    record.tags = uniqueSorted(record.tags);
    // And `tagged` the same way: a permissive conversion reads `"tagged":"nope"`
    // as 0, a row `recent` and `--since` can never judge, and `"tagged":true` as
    // 1, a file tagged in January 1970. Both would arrive at exit 0.
    const QJsonValue taggedValue = object.value(QStringLiteral("tagged"));
    if (taggedValue.isUndefined())
        return record;
    if (!taggedValue.isDouble()) {
        if (error)
            *error = QStringLiteral("index row has a non-numeric tagged field");
        return {};
    }
    // An undated row leaves the field out, so a zero or a negative one is a
    // stamp nothing here wrote.
    record.tagged = taggedValue.toInteger();
    if (record.tagged <= 0) {
        if (error)
            *error = QStringLiteral("index row has a tagged field that is not a positive whole number");
        return {};
    }
    return record;
}

} // namespace omafiles
