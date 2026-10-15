#include "Portal.h"
#include <QDebug>
#include "../core/Paths.h"
#include <KWindowSystem>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>
#include <QWindow>
#include <QtConcurrent>
#include <sys/stat.h>
namespace {
struct Rule {
    uint type;
    QString pattern;
};
using Rules = QList<Rule>;
struct Filter {
    QString name;
    Rules rules;
};
using Filters = QList<Filter>;
struct Option {
    QString id, label;
};
using Options = QList<Option>;
struct Choice {
    QString id, label;
    Options options;
    QString selected;
};
using Choices = QList<Choice>;
QDBusArgument& operator<<(QDBusArgument& a, const Rule& v)
{
    a.beginStructure();
    a << v.type << v.pattern;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, Rule& v)
{
    a.beginStructure();
    a >> v.type >> v.pattern;
    a.endStructure();
    return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const Filter& v)
{
    a.beginStructure();
    a << v.name << v.rules;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, Filter& v)
{
    a.beginStructure();
    a >> v.name >> v.rules;
    a.endStructure();
    return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const Option& v)
{
    a.beginStructure();
    a << v.id << v.label;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, Option& v)
{
    a.beginStructure();
    a >> v.id >> v.label;
    a.endStructure();
    return a;
}
QDBusArgument& operator<<(QDBusArgument& a, const Choice& v)
{
    a.beginStructure();
    a << v.id << v.label << v.options << v.selected;
    a.endStructure();
    return a;
}
const QDBusArgument& operator>>(const QDBusArgument& a, Choice& v)
{
    a.beginStructure();
    a >> v.id >> v.label >> v.options >> v.selected;
    a.endStructure();
    return a;
}
bool typed(const QVariant& value, const char* signature)
{
    return value.metaType() == QMetaType::fromType<QDBusArgument>()
        && value.value<QDBusArgument>().currentSignature() == QLatin1String(signature);
}
bool basename(const QString& name)
{
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/')
        && !name.contains(QChar::Null);
}
QVariantMap filterMap(const Filter& f)
{
    QVariantList rules;
    for (const auto& r : f.rules)
        rules << QVariantMap { { "type", r.type }, { "pattern", r.pattern } };
    return { { "name", f.name }, { "rules", rules } };
}
Filter mapFilter(const QVariantMap& map)
{
    Filter f { map.value("name").toString(), { } };
    for (const auto& v : map.value("rules").toList()) {
        auto r = v.toMap();
        f.rules << Rule { r.value("type").toUInt(), r.value("pattern").toString() };
    }
    return f;
}
}
Q_DECLARE_METATYPE(Rule)
Q_DECLARE_METATYPE(Rules)
Q_DECLARE_METATYPE(Filter)
Q_DECLARE_METATYPE(Filters)
Q_DECLARE_METATYPE(Option)
Q_DECLARE_METATYPE(Options)
Q_DECLARE_METATYPE(Choice)
Q_DECLARE_METATYPE(Choices)
namespace omafiles {
PortalRequest::PortalRequest(Portal* portal)
    : QObject(portal)
    , m_portal(portal)
{
}
bool PortalRequest::parse(const QVariantMap& o, QString* reason)
{
    auto fail = [&](const QString& s) {
        *reason = s;
        return false;
    };
    const QStringList strings { "accept_label", "current_name" };
    for (const auto& key : strings)
        if (o.contains(key) && o[key].metaType() != QMetaType::fromType<QString>())
            return fail("Invalid string option: " + key);
    for (const auto& key : { "modal", "multiple", "directory" })
        if (o.contains(key) && o[key].metaType() != QMetaType::fromType<bool>())
            return fail(QStringLiteral("Invalid boolean option: ") + key);
    acceptLabel = o.value("accept_label", kind == "open" ? "Open" : "Save").toString();
    modal = o.value("modal", true).toBool();
    multiple = o.value("multiple", false).toBool();
    directory = o.value("directory", false).toBool();
    currentName = o.value("current_name").toString();
    if (!currentName.isEmpty() && !basename(currentName))
        return fail("The suggested filename must be a single filename.");
    for (const auto& key : { "current_folder", "current_file" }) {
        if (!o.contains(key))
            continue;
        if (o[key].metaType() != QMetaType::fromType<QByteArray>())
            return fail(QStringLiteral("Invalid byte path option: ") + key);
        QByteArray bytes = o[key].toByteArray();
        if (bytes.isEmpty() || !bytes.endsWith('\0'))
            return fail("A byte path must end with NUL.");
        bytes.chop(1);
        QString path;
        if (!nameFromBytes(bytes, &path) || !path.startsWith('/'))
            return fail("A byte path must be an absolute UTF-8 path.");
        if (QByteArray(key) == "current_file")
            currentFile = path;
        else
            currentFolder = path;
    }
    if (!currentFile.isEmpty()) {
        QString name;
        currentFolder = typedDirectory(currentFile, &name);
        currentName = name;
    }
    if (currentFolder.isEmpty())
        currentFolder = homeDirectory();
    if (o.contains("filters")) {
        if (!typed(o["filters"], "a(sa(us))"))
            return fail("Invalid filters.");
        const auto parsed = qdbus_cast<Filters>(o["filters"]);
        if (parsed.size() > 128)
            return fail("Too many filters.");
        for (const auto& f : parsed) {
            if (f.rules.size() > 128)
                return fail("Too many filter rules.");
            for (const auto& r : f.rules)
                if (r.type > 1 || r.pattern.isEmpty())
                    return fail("Invalid filter rule.");
            filters << filterMap(f);
        }
    }
    if (!filters.isEmpty())
        m_selectedFilter = 0;
    if (o.contains("current_filter")) {
        if (!typed(o["current_filter"], "(sa(us))"))
            return fail("Invalid current filter.");
        const auto selected = qdbus_cast<Filter>(o["current_filter"]);
        if (selected.rules.size() > 128)
            return fail("Too many filter rules.");
        for (const auto& rule : selected.rules)
            if (rule.type > 1 || rule.pattern.isEmpty())
                return fail("Invalid filter rule.");
        const auto map = filterMap(selected);
        if (filters.isEmpty())
            filters << map;
        m_selectedFilter = filters.indexOf(map);
        if (m_selectedFilter < 0)
            return fail("The current filter is not in the filter list.");
    }
    if (o.contains("choices")) {
        if (!typed(o["choices"], "a(ssa(ss)s)"))
            return fail("Invalid choices.");
        const auto choices = qdbus_cast<Choices>(o["choices"]);
        QSet<QString> ids;
        if (choices.size() > 128)
            return fail("Too many choices.");
        for (const auto& c : choices) {
            if (c.id.isEmpty() || ids.contains(c.id) || c.options.size() > 128)
                return fail("Invalid choice identifier or options.");
            ids.insert(c.id);
            QVariantList options;
            QSet<QString> values;
            for (const auto& v : c.options) {
                if (v.id.isEmpty() || values.contains(v.id))
                    return fail("Invalid choice value.");
                values.insert(v.id);
                options << QVariantMap { { "id", v.id }, { "label", v.label } };
            }
            QString selected = c.selected;
            if (options.isEmpty()) {
                if (selected.isEmpty())
                    selected = "false";
                if (selected != "true" && selected != "false")
                    return fail("Invalid boolean choice.");
            } else {
                if (selected.isEmpty())
                    selected = c.options.first().id;
                if (!values.contains(selected))
                    return fail("Invalid initial choice.");
            }
            m_choices << QVariantMap { { "id", c.id }, { "label", c.label }, { "options", options },
                { "selected", selected } };
        }
    }
    if (kind == "savefiles") {
        if (!o.contains("files") || !typed(o["files"], "aay"))
            return fail("SaveFiles requires byte filenames.");
        const auto names = qdbus_cast<QList<QByteArray>>(o["files"]);
        if (names.isEmpty() || names.size() > 4096)
            return fail("Invalid number of filenames.");
        for (auto bytes : names) {
            if (!bytes.endsWith('\0'))
                return fail("A filename must end with NUL.");
            bytes.chop(1);
            QString name;
            if (!nameFromBytes(bytes, &name) || !basename(name))
                return fail("SaveFiles requires UTF-8 filenames without path components.");
            files << name;
        }
    }
    if (!parentWindow.isEmpty() && !parentWindow.startsWith("wayland:")) {
        bool ok = false;
        const auto id = parentWindow.mid(4).toULongLong(&ok, 16);
        if (!parentWindow.startsWith("x11:") || !ok || !id)
            return fail("Invalid parent window identifier.");
    }
    if (parentWindow == "wayland:")
        return fail("Invalid parent window identifier.");
    return true;
}
void PortalRequest::prepareFilters()
{
    if (filters.isEmpty()) {
        m_filtersReady = true;
        return;
    }
    const auto requestedFilters = filters;
    const auto cancelled = m_cancelled;
    auto* patterns = new QFutureWatcher<QList<QList<QRegularExpression>>>(this);
    connect(patterns, &QFutureWatcher<QList<QList<QRegularExpression>>>::finished, this,
        [this, patterns] {
            m_patterns = patterns->result();
            m_filtersReady = true;
            patterns->deleteLater();
            emit changed();
        });
    patterns->setFuture(QtConcurrent::run([requestedFilters, cancelled] {
        QList<QList<QRegularExpression>> compiled;
        QMimeDatabase database;
        QList<QMimeType> mimeTypes;
        QHash<QString, QRegularExpression> cache;
        for (const auto& value : requestedFilters) {
            if (cancelled->load())
                return compiled;
            QList<QRegularExpression> patterns;
            QSet<QString> heldPatterns;
            for (const auto& ruleValue : value.toMap().value("rules").toList()) {
                if (cancelled->load())
                    return compiled;
                const auto rule = ruleValue.toMap();
                const auto pattern = rule.value("pattern").toString();
                QStringList globs;
                if (rule.value("type").toUInt() == 0)
                    globs << pattern;
                else {
                    if (mimeTypes.isEmpty())
                        mimeTypes = database.allMimeTypes();
                    for (const auto& type : mimeTypes) {
                        if (cancelled->load())
                            return compiled;
                        const bool match = pattern.endsWith("/*")
                            ? type.name().startsWith(pattern.chopped(1))
                            : type.inherits(pattern);
                        if (match)
                            globs << type.globPatterns();
                    }
                }
                const auto flags = rule.value("type").toUInt() == 0
                    ? QRegularExpression::NoPatternOption
                    : QRegularExpression::CaseInsensitiveOption;
                for (const auto& glob : globs) {
                    if (cancelled->load())
                        return compiled;
                    const auto key = QString::number(uint(flags)) + ":" + glob;
                    if (!cache.contains(key))
                        cache.insert(key,
                            QRegularExpression(
                                QRegularExpression::wildcardToRegularExpression(glob), flags));
                    if (!heldPatterns.contains(key)) {
                        heldPatterns.insert(key);
                        patterns << cache.value(key);
                    }
                }
            }
            compiled << patterns;
        }
        return compiled;
    }));
}
void PortalRequest::setSelectedFilter(int index)
{
    if (index >= 0 && index < filters.size() && index != m_selectedFilter) {
        m_selectedFilter = index;
        emit changed();
    }
}
QVariantList PortalRequest::choiceList() const
{
    QVariantList definitions;
    for (const auto& value : m_choices) {
        auto choice = value.toMap();
        choice.remove("selected");
        definitions << choice;
    }
    return definitions;
}
QVariantMap PortalRequest::choiceValues() const
{
    QVariantMap values;
    for (const auto& value : m_choices) {
        const auto choice = value.toMap();
        values.insert(choice["id"].toString(), choice["selected"]);
    }
    return values;
}
void PortalRequest::setChoice(const QString& id, const QString& value)
{
    for (int i = 0; i < m_choices.size(); ++i) {
        auto c = m_choices[i].toMap();
        if (c["id"].toString() != id)
            continue;
        bool valid = false;
        const auto options = c["options"].toList();
        if (options.isEmpty())
            valid = value == "true" || value == "false";
        else
            for (const auto& v : options)
                valid |= v.toMap()["id"].toString() == value;
        if (valid) {
            c["selected"] = value;
            m_choices[i] = c;
            emit changed();
        }
        return;
    }
}
bool PortalRequest::matches(const QString& filename) const
{
    if (m_selectedFilter < 0 || !m_filtersReady)
        return true;
    for (const auto& pattern : m_patterns[m_selectedFilter])
        if (pattern.match(filename).hasMatch())
            return true;
    return false;
}
void PortalRequest::attachParent(QWindow* window)
{
    if (!window)
        return;
    window->setModality(modal ? Qt::WindowModal : Qt::NonModal);
    if (parentWindow.startsWith("wayland:"))
        KWindowSystem::setMainWindow(window, parentWindow.mid(8));
    else if (parentWindow.startsWith("x11:"))
        KWindowSystem::setMainWindow(window, WId(parentWindow.mid(4).toULongLong(nullptr, 16)));
}
struct Validation {
    QString error;
    QStringList paths, overwrite;
};
void PortalRequest::finish(const QStringList& paths)
{
    if (busy || m_completed)
        return;
    if (paths.isEmpty() || paths.size() > 4096 || (kind != "open" && paths.size() != 1)
        || (kind == "open" && !multiple && paths.size() != 1)) {
        error = "Choose the required number of files or folders.";
        emit changed();
        return;
    }
    busy = true;
    error.clear();
    overwritePaths.clear();
    emit changed();
    const auto requestKind = kind;
    const auto names = files;
    const bool folders = directory;
    const auto cancelled = m_cancelled;
    auto* watcher = new QFutureWatcher<Validation>(this);
    connect(watcher, &QFutureWatcher<Validation>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (m_completed)
            return;
        busy = false;
        error = result.error;
        m_pending = result.paths;
        overwritePaths = result.overwrite;
        emit changed();
        if (error.isEmpty() && overwritePaths.isEmpty())
            confirmOverwrite(true);
    });
    watcher->setFuture(QtConcurrent::run([paths, requestKind, names, folders, cancelled] {
        Validation result;
        auto inspect = [&](const QString& p, struct stat* s, bool follow = true) {
            QString why;
            const auto state = statState(p, &why, follow, s);
            if (state == PathState::Unknown)
                result.error = "Cannot inspect " + spelled(p) + ": " + why;
            return state;
        };
        for (const auto& p : paths) {
            if (cancelled->load())
                return result;
            QString named;
            if (!p.startsWith('/') || !nameFromBytes(p.toUtf8(), &named) || named != p) {
                result.error = "Choose an absolute UTF-8 path.";
                break;
            }
            struct stat st { };
            const auto state = inspect(p, &st);
            if (!result.error.isEmpty())
                break;
            if (requestKind == "open") {
                if (state != PathState::Present) {
                    result.error = "The selected path does not exist: " + spelled(p);
                    break;
                }
                if (folders != bool(S_ISDIR(st.st_mode))) {
                    result.error = folders ? "Choose a folder." : "Choose a file.";
                    break;
                }
                result.paths << p;
            } else if (requestKind == "save") {
                QString name;
                const auto parent = typedDirectory(p, &name);
                struct stat parentStat { };
                if (!basename(name) || inspect(parent, &parentStat) != PathState::Present
                    || !S_ISDIR(parentStat.st_mode)) {
                    if (result.error.isEmpty())
                        result.error = "The destination folder is unavailable.";
                    break;
                }
                if (state == PathState::Present && S_ISDIR(st.st_mode)) {
                    result.error = "The destination is a folder.";
                    break;
                }
                struct stat linkStat { };
                const auto linkState = inspect(p, &linkStat, false);
                if (!result.error.isEmpty())
                    break;
                if (linkState == PathState::Present)
                    result.overwrite << p;
                result.paths << p;
            } else {
                if (state != PathState::Present || !S_ISDIR(st.st_mode)) {
                    result.error = "Choose an existing destination folder.";
                    break;
                }
                QSet<QString> used;
                for (const auto& name : names) {
                    if (cancelled->load())
                        return result;
                    QString candidate = name;
                    int suffix = 1;
                    while (used.contains(foldedName(candidate))
                        || inspect(p + "/" + candidate, &st, false) != PathState::Missing) {
                        if (cancelled->load())
                            return result;
                        if (!result.error.isEmpty())
                            break;
                        const int dot = name.lastIndexOf('.');
                        const int split = dot > 0 ? dot : name.size();
                        candidate = name.left(split) + " (" + QString::number(suffix++) + ")"
                            + name.mid(split);
                        if (suffix > 100000) {
                            result.error = "Cannot find an available filename.";
                            break;
                        }
                    }
                    if (!result.error.isEmpty())
                        break;
                    used.insert(foldedName(candidate));
                    result.paths << p + "/" + candidate;
                }
            }
        }
        return result;
    }));
}
void PortalRequest::confirmOverwrite(bool accept)
{
    if (busy || m_completed || m_pending.isEmpty())
        return;
    if (!accept) {
        m_pending.clear();
        overwritePaths.clear();
        emit changed();
        return;
    }
    QStringList uris;
    for (const auto& p : m_pending)
        uris << QUrl::fromLocalFile(p).toString(QUrl::FullyEncoded);
    QVariantMap results { { "uris", uris } };
    if (kind == "open")
        results.insert("writable", false);
    if (m_selectedFilter >= 0)
        results.insert(
            "current_filter", QVariant::fromValue(mapFilter(filters[m_selectedFilter].toMap())));
    Options choices;
    for (const auto& c : m_choices) {
        const auto map = c.toMap();
        choices << Option { map["id"].toString(), map["selected"].toString() };
    }
    if (!choices.isEmpty())
        results.insert("choices", QVariant::fromValue(choices));
    m_portal->complete(this, 0, results);
}
void PortalRequest::cancel()
{
    m_portal->complete(this, 1);
}
Portal::Portal(QObject* parent)
    : QDBusVirtualObject(parent)
    , m_connection(QString())
{
    qDBusRegisterMetaType<Rule>();
    qDBusRegisterMetaType<Rules>();
    qDBusRegisterMetaType<Filter>();
    qDBusRegisterMetaType<Filters>();
    qDBusRegisterMetaType<Option>();
    qDBusRegisterMetaType<Options>();
    qDBusRegisterMetaType<Choice>();
    qDBusRegisterMetaType<Choices>();
    qDBusRegisterMetaType<QList<QByteArray>>();
}
Portal::~Portal()
{
    if (m_started) {
        while (!m_requests.isEmpty())
            complete(m_requests.first(), 2);
        m_connection.unregisterObject("/org/freedesktop/portal/desktop");
        m_connection.unregisterService("org.freedesktop.impl.portal.desktop.omafiles");
    }
}
bool Portal::start(const QDBusConnection& connection)
{
    if (m_started)
        return true;
    m_connection = connection;
    if (!connection.isConnected()
        || !m_connection.registerService("org.freedesktop.impl.portal.desktop.omafiles"))
        return false;
    if (!m_connection.registerVirtualObject("/org/freedesktop/portal/desktop", this)) {
        m_connection.unregisterService("org.freedesktop.impl.portal.desktop.omafiles");
        return false;
    }
    m_connection.connect("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", this, SLOT(ownerChanged(QString, QString, QString)));
    m_started = true;
    return true;
}
QString Portal::introspect(const QString& path) const
{
    if (path != "/org/freedesktop/portal/desktop")
        return "<interface name='org.freedesktop.impl.portal.Request'><method "
               "name='Close'/></interface>";
    QString xml = "<interface name='org.freedesktop.impl.portal.FileChooser'><property "
                  "name='version' type='u' access='read'/>";
    for (const auto& name : { "OpenFile", "SaveFile", "SaveFiles" })
        xml += QString("<method name='%1'><arg type='o' direction='in'/><arg type='s' "
                       "direction='in'/><arg type='s' direction='in'/><arg type='s' "
                       "direction='in'/><arg type='a{sv}' direction='in'/><arg type='u' "
                       "direction='out'/><arg type='a{sv}' direction='out'/></method>")
                   .arg(name);
    return xml + "</interface>";
}
bool Portal::handleMessage(const QDBusMessage& message, const QDBusConnection& connection)
{
    auto reject = [&](const QString& reason) {
        connection.send(message.createErrorReply(QDBusError::InvalidArgs, reason));
        return true;
    };
    if (message.interface() == "org.freedesktop.DBus.Properties" && message.member() == "Get"
        && message.arguments()
            == QVariantList {
                QString("org.freedesktop.impl.portal.FileChooser"), QString("version") }) {
        connection.send(message.createReply(QVariant::fromValue(QDBusVariant(uint(4)))));
        return true;
    }
    if (message.interface() == "org.freedesktop.DBus.Properties" && message.member() == "GetAll"
        && message.arguments()
            == QVariantList { QString("org.freedesktop.impl.portal.FileChooser") }) {
        connection.send(message.createReply(QVariantMap { { "version", uint(4) } }));
        return true;
    }
    if (message.interface() == "org.freedesktop.impl.portal.Request"
        && message.member() == "Close") {
        for (auto* r : m_requests)
            if (r->m_handle == message.path()) {
                if (!message.arguments().isEmpty())
                    return reject("Close takes no arguments.");
                if (message.service() != r->m_sender) {
                    connection.send(message.createErrorReply(
                        QDBusError::AccessDenied, "This request belongs to another sender."));
                    return true;
                }
                connection.send(message.createReply());
                complete(r, 2);
                return true;
            }
        return false;
    }
    if (message.interface() != "org.freedesktop.impl.portal.FileChooser"
        || message.path() != "/org/freedesktop/portal/desktop")
        return false;
    if (message.member() != "OpenFile" && message.member() != "SaveFile"
        && message.member() != "SaveFiles")
        return false;
    if (message.signature() != "osssa{sv}")
        return reject(
            "Expected a request handle, application ID, parent window, title and options.");
    const auto args = message.arguments();
    const auto optionsArgument = args[4].value<QDBusArgument>();
    QVariantMap options;
    optionsArgument >> options;

    const auto handle = args[0].value<QDBusObjectPath>().path();
    if (!handle.startsWith("/org/freedesktop/portal/desktop/request/") || m_requests.size() >= 64)
        return reject("Invalid request handle or too many pending requests.");
    for (auto* r : m_requests)
        if (r->m_handle == handle)
            return reject("The request handle is already in use.");
    auto* request = new PortalRequest(this);
    request->kind = message.member() == "OpenFile" ? "open"
        : message.member() == "SaveFile"           ? "save"
                                                   : "savefiles";
    request->appId = args[1].toString();
    request->parentWindow = args[2].toString();
    request->title = args[3].toString();
    request->m_reply = message.createReply();
    request->m_sender = message.service();
    request->m_handle = handle;
    QString reason;
    if (!request->parse(options, &reason)) {
        delete request;
        return reject(reason);
    }
    if (!m_connection.registerVirtualObject(handle, this)) {
        delete request;
        return reject("Cannot export the request handle.");
    }
    m_requests << request;
    if (m_requests.size() == 1) {
        request->prepareFilters();
        emit currentChanged();
    }
    return true;
}
void Portal::complete(PortalRequest* request, uint response, const QVariantMap& results)
{
    if (request->m_completed)
        return;
    const bool wasCurrent = current() == request;
    request->m_completed = true;
    request->m_cancelled->store(true);
    request->m_reply.setArguments(QVariantList { response, results });
    m_connection.send(request->m_reply);
    m_connection.unregisterObject(request->m_handle);
    m_requests.removeOne(request);
    request->deleteLater();
    if (wasCurrent) {
        if (current())
            current()->prepareFilters();
        emit currentChanged();
    }
}
void Portal::ownerChanged(const QString& name, const QString&, const QString& newOwner)
{
    if (!name.startsWith(':') || !newOwner.isEmpty())
        return;
    const auto requests = m_requests;
    for (auto* r : requests)
        if (r->m_sender == name)
            complete(r, 2);
}
}
