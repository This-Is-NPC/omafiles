#include "Record.h"

#include "Tag.h"

#include <QJsonArray>
#include <QJsonValue>

namespace omafiles {

QJsonObject Record::toJson() const
{
    QJsonArray tagArray;
    for (const QString &tag : tags)
        tagArray.append(tag);
    QJsonObject object{
        {QStringLiteral("path"), path},
        {QStringLiteral("tags"), tagArray},
    };
    if (tagged > 0)
        object.insert(QStringLiteral("tagged"), tagged);
    return object;
}

Record Record::fromJson(const QJsonObject &object, QString *error)
{
    Record record;
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
