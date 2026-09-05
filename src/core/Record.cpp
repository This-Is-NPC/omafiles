#include "Record.h"

#include "Roots.h"
#include "Tag.h"

#include <QJsonArray>
#include <QJsonValue>

namespace omafiles {

// Written under whichever root holds it now, and not under the one it was read
// from: declaring a root is what migrates the rows beneath it, on the next
// write, with no verb to run and nothing to convert by hand. A row under no
// declared root keeps its absolute path, which is what every row was before
// this existed and what every row on a machine that declares nothing stays.
QJsonObject Record::toJson() const
{
    QJsonArray tagArray;
    for (const QString &tag : tags)
        tagArray.append(tag);

    QString rootId;
    QString rest;
    splitByRoot(path, &rootId, &rest);
    if (rootId.isEmpty()) {
        // A row whose root is declared no longer, kept exactly as it was read.
        // Anything else would rewrite it as an absolute path built from a mount
        // point that is not there -- which is how the tags of an archive get
        // filed under a directory that does not exist.
        rootId = root;
        rest = relative;
    }

    QJsonObject object{{QStringLiteral("tags"), tagArray}};
    if (rootId.isEmpty()) {
        object.insert(QStringLiteral("path"), path);
    } else {
        object.insert(QStringLiteral("root"), rootId);
        object.insert(QStringLiteral("path"), rest);
    }
    if (tagged > 0)
        object.insert(QStringLiteral("tagged"), tagged);
    return object;
}

Record Record::fromJson(const QJsonObject &object, QString *error)
{
    Record record;
    // `root` names a declaration, and one that is not there is a row nothing
    // can place: refused by name, the way a line that will not parse is, rather
    // than guessed at. Putting the line back in the config is the repair, and
    // it is a repair that loses nothing.
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
                *error = QStringLiteral("index row names the root %1, which is not declared; "
                                        "put it back in the roots file or the rows under it "
                                        "cannot be placed")
                             .arg(record.root);
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
        record.tags.append(value.toString());
    }
    record.tags = uniqueSorted(record.tags);
    // And `tagged` the same way: a permissive conversion reads `"tagged":"nope"`
    // as 0, a row `recent` and `--since` can never judge, and `"tagged":true` as
    // 1, a file tagged in January 1970. Both would arrive at exit 0.
    const QJsonValue taggedValue = object.value(QStringLiteral("tagged"));
    if (!taggedValue.isUndefined() && !taggedValue.isDouble()) {
        if (error)
            *error = QStringLiteral("index row has a non-numeric tagged field");
        return {};
    }
    record.tagged = taggedValue.toInteger();
    return record;
}

} // namespace omafiles
