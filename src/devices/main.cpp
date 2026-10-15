#include <gio/gio.h>
#include <glib-unix.h>
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {
// Simple JSON value type for encoding/decoding
class JsonValue {
public:
    enum Type { Null, Bool, String, Array, Object };
    Type type;
    bool boolValue = false;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::unordered_map<std::string, JsonValue> objectValue;

    JsonValue() : type(Null) {}
    explicit JsonValue(bool b) : type(Bool), boolValue(b) {}
    explicit JsonValue(const std::string &s) : type(String), stringValue(s) {}
    explicit JsonValue(const char *s) : type(String), stringValue(s ? s : "") {}

    JsonValue &operator[](const std::string &key) {
        if (type != Object) type = Object;
        return objectValue[key];
    }

    const JsonValue &value(const std::string &key) const {
        auto it = objectValue.find(key);
        if (it != objectValue.end()) return it->second;
        static const JsonValue null;
        return null;
    }

    bool isObject() const { return type == Object; }
    bool isBool() const { return type == Bool; }
    bool isArray() const { return type == Array; }
    bool isString() const { return type == String; }
    bool isNull() const { return type == Null; }
    bool toBool() const { return boolValue; }
    std::string toString() const { return stringValue; }
    size_t size() const { return arrayValue.size(); }
};

// JSON encoder
static std::string escapeJsonString(const std::string &input) {
    std::string result;
    for (unsigned char c : input) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else if (c == '\b') result += "\\b";
        else if (c == '\f') result += "\\f";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else if (c < 0x20) {
            char buf[7];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            result += buf;
        } else {
            result += c;
        }
    }
    return result;
}

static std::string jsonEncode(const JsonValue &value);

static std::string jsonEncodeArray(const std::vector<JsonValue> &arr) {
    std::string result = "[";
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i > 0) result += ",";
        result += jsonEncode(arr[i]);
    }
    result += "]";
    return result;
}

static std::string jsonEncodeObject(const std::unordered_map<std::string, JsonValue> &obj) {
    std::string result = "{";
    bool first = true;
    for (const auto &pair : obj) {
        if (!first) result += ",";
        first = false;
        result += "\"" + escapeJsonString(pair.first) + "\":" + jsonEncode(pair.second);
    }
    result += "}";
    return result;
}

static std::string jsonEncode(const JsonValue &value) {
    switch (value.type) {
        case JsonValue::Null: return "null";
        case JsonValue::Bool: return value.boolValue ? "true" : "false";
        case JsonValue::String: return "\"" + escapeJsonString(value.stringValue) + "\"";
        case JsonValue::Array: return jsonEncodeArray(value.arrayValue);
        case JsonValue::Object: return jsonEncodeObject(value.objectValue);
    }
    return "null";
}

static void send(const JsonValue &message) {
    std::string json = jsonEncode(message);
    json += '\n';
    if (std::fwrite(json.data(), 1, json.size(), stdout) != json.size() || std::fflush(stdout))
        std::exit(1);
}

// JSON decoder - simple recursive descent parser
class JsonParser {
public:
    const std::string &input;
    size_t pos = 0;

    explicit JsonParser(const std::string &s) : input(s) {}

    void skipWhitespace() {
        while (pos < input.size() && std::isspace(input[pos])) ++pos;
    }

    JsonValue parse() {
        skipWhitespace();
        if (pos >= input.size()) return JsonValue();
        char c = input[pos];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return parseString();
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        return JsonValue();
    }

private:
    JsonValue parseNull() {
        if (input.substr(pos, 4) == "null") {
            pos += 4;
            return JsonValue();
        }
        return JsonValue();
    }

    JsonValue parseBool() {
        if (input.substr(pos, 4) == "true") {
            pos += 4;
            return JsonValue(true);
        }
        if (input.substr(pos, 5) == "false") {
            pos += 5;
            return JsonValue(false);
        }
        return JsonValue();
    }

    JsonValue parseString() {
        if (input[pos] != '"') return JsonValue();
        ++pos;
        std::string result;
        while (pos < input.size() && input[pos] != '"') {
            if (input[pos] == '\\') {
                ++pos;
                if (pos >= input.size()) return JsonValue();
                char c = input[pos];
                if (c == '"') result += '"';
                else if (c == '\\') result += '\\';
                else if (c == '/') result += '/';
                else if (c == 'b') result += '\b';
                else if (c == 'f') result += '\f';
                else if (c == 'n') result += '\n';
                else if (c == 'r') result += '\r';
                else if (c == 't') result += '\t';
                else if (c == 'u') {
                    if (pos + 4 >= input.size()) return JsonValue();
                    ++pos;
                    unsigned int codepoint = 0;
                    for (int i = 0; i < 4; ++i) {
                        codepoint = codepoint * 16;
                        char h = input[pos];
                        if (h >= '0' && h <= '9') codepoint += h - '0';
                        else if (h >= 'a' && h <= 'f') codepoint += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') codepoint += h - 'A' + 10;
                        else return JsonValue();
                        ++pos;
                    }
                    --pos;
                    if (codepoint < 0x80) {
                        result += static_cast<char>(codepoint);
                    } else if (codepoint < 0x800) {
                        result += static_cast<char>(0xc0 | (codepoint >> 6));
                        result += static_cast<char>(0x80 | (codepoint & 0x3f));
                    } else {
                        result += static_cast<char>(0xe0 | (codepoint >> 12));
                        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
                        result += static_cast<char>(0x80 | (codepoint & 0x3f));
                    }
                } else {
                    return JsonValue();
                }
                ++pos;
            } else {
                result += input[pos];
                ++pos;
            }
        }
        if (pos >= input.size() || input[pos] != '"') return JsonValue();
        ++pos;
        JsonValue v(result);
        v.type = JsonValue::String;
        return v;
    }

    JsonValue parseArray() {
        if (input[pos] != '[') return JsonValue();
        ++pos;
        JsonValue result;
        result.type = JsonValue::Array;
        skipWhitespace();
        if (pos < input.size() && input[pos] == ']') {
            ++pos;
            return result;
        }
        while (pos < input.size()) {
            skipWhitespace();
            result.arrayValue.push_back(parse());
            skipWhitespace();
            if (pos >= input.size()) return JsonValue();
            if (input[pos] == ']') {
                ++pos;
                return result;
            }
            if (input[pos] == ',') {
                ++pos;
            } else {
                return JsonValue();
            }
        }
        return JsonValue();
    }

    JsonValue parseObject() {
        if (input[pos] != '{') return JsonValue();
        ++pos;
        JsonValue result;
        result.type = JsonValue::Object;
        skipWhitespace();
        if (pos < input.size() && input[pos] == '}') {
            ++pos;
            return result;
        }
        while (pos < input.size()) {
            skipWhitespace();
            if (input[pos] != '"') return JsonValue();
            JsonValue key = parseString();
            if (!key.isString()) return JsonValue();
            skipWhitespace();
            if (pos >= input.size() || input[pos] != ':') return JsonValue();
            ++pos;
            skipWhitespace();
            JsonValue value = parse();
            result.objectValue[key.stringValue] = value;
            skipWhitespace();
            if (pos >= input.size()) return JsonValue();
            if (input[pos] == '}') {
                ++pos;
                return result;
            }
            if (input[pos] == ',') {
                ++pos;
            } else {
                return JsonValue();
            }
        }
        return JsonValue();
    }
};

struct Device {
    GVolume *volume = nullptr;
    GMount *mount = nullptr;
    GDrive *drive = nullptr;
    ~Device() { g_clear_object(&volume); g_clear_object(&mount); g_clear_object(&drive); }
};

struct Monitor {
    GVolumeMonitor *volumes = nullptr;
    GMainLoop *loop = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Device>> devices;
    std::string input;
    std::string pending, action, interactionReason;
    GCancellable *cancel = nullptr;
    bool scheduled = false;
    int exitCode = 0;

    void snapshot();
    void operate(const std::string &id, const std::string &verb);
    void finish(bool ok, const std::string &reason);
};

static bool isValidUtf8(const std::string &s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c == 0) return false;
        if (c < 0x80) continue;
        if ((c & 0xe0) == 0xc0) {
            if (i + 1 >= s.size()) return false;
            i += 1;
        } else if ((c & 0xf0) == 0xe0) {
            if (i + 2 >= s.size()) return false;
            i += 2;
        } else if ((c & 0xf8) == 0xf0) {
            if (i + 3 >= s.size()) return false;
            i += 3;
        } else {
            return false;
        }
    }
    return true;
}

static std::string take(char *value) {
    std::string result = (value ? value : "");
    g_free(value);
    if (isValidUtf8(result)) return result;
    return {};
}

static std::string key(GVolume *volume) {
    std::string id = take(g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
    if (id.empty()) {
        GFile *root = g_volume_get_activation_root(volume);
        if (root) {
            id = take(g_file_get_uri(root));
            g_object_unref(root);
        }
    }
    if (id.empty()) id = take(g_volume_get_uuid(volume));
    if (id.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "volume:%p", (void *)volume);
        id = buf;
    }
    return id;
}

static std::string location(GMount *mount) {
    if (!mount) return {};
    GFile *root = g_mount_get_root(mount);
    std::string path = take(g_file_get_path(root));
    g_object_unref(root);
    return path.empty() || path[0] != '/' ? std::string() : path;
}

void Monitor::snapshot() {
    std::unordered_map<std::string, std::shared_ptr<Device>> next;
    std::vector<JsonValue> sorted;

    GList *list = g_volume_monitor_get_volumes(volumes);
    for (GList *node = list; node; node = node->next) {
        auto item = std::make_shared<Device>();
        item->volume = G_VOLUME(g_object_ref(node->data));
        item->drive = g_volume_get_drive(item->volume);
        item->mount = g_volume_get_mount(item->volume);

        GFile *activation = g_volume_get_activation_root(item->volume);
        std::string uri = activation ? take(g_file_get_uri(activation)) : "";
        if (activation) g_object_unref(activation);

        bool phone = false;
        for (const char *scheme : {"mtp:", "gphoto2:", "afc:"}) phone = phone || uri.rfind(scheme, 0) == 0;
        if (!phone && (!item->drive || !g_drive_is_removable(item->drive))) continue;

        std::string id = key(item->volume), name = take(g_volume_get_name(item->volume));
        if (name.empty() || next.count(id)) continue;

        std::string path = location(item->mount);

        JsonValue row;
        row["id"] = JsonValue(id);
        row["name"] = JsonValue(name);
        row["path"] = JsonValue(path);
        row["phone"] = JsonValue(phone);
        row["mounted"] = JsonValue(item->mount != nullptr);
        row["canMount"] = JsonValue(g_volume_can_mount(item->volume));
        row["canUnmount"] = JsonValue(item->mount && g_mount_can_unmount(item->mount));
        row["canEject"] = JsonValue(g_volume_can_eject(item->volume) || (item->drive && (g_drive_can_eject(item->drive) || g_drive_can_stop(item->drive)))
            || (item->mount && g_mount_can_eject(item->mount)));

        sorted.push_back(row);
        next[id] = item;
    }
    g_list_free_full(list, g_object_unref);

    std::sort(sorted.begin(), sorted.end(), [](const JsonValue &a, const JsonValue &b) {
        std::string left = a.value("name").toString();
        std::string right = b.value("name").toString();
        std::transform(left.begin(), left.end(), left.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(right.begin(), right.end(), right.begin(), [](unsigned char c) { return std::tolower(c); });
        if (left != right) return left < right;
        return a.value("id").toString() < b.value("id").toString();
    });

    JsonValue message;
    message["devices"].type = JsonValue::Array;
    message["devices"].arrayValue = sorted;

    devices.swap(next);
    send(message);
}

static void changed(GVolumeMonitor *, gpointer, gpointer data) {
    auto *monitor = static_cast<Monitor *>(data);
    if (monitor->scheduled) return;
    monitor->scheduled = true;
    g_idle_add([](gpointer data) -> gboolean {
        auto *monitor = static_cast<Monitor *>(data);
        monitor->scheduled = false;
        monitor->snapshot();
        return G_SOURCE_REMOVE;
    }, monitor);
}

void Monitor::finish(bool ok, const std::string &reason) {
    snapshot();
    std::string id = pending, verb = action;
    std::string path, message = reason;

    if (ok && verb == "open") {
        auto it = devices.find(id);
        if (it != devices.end()) path = location(it->second->mount);
        if (path.empty()) {
            ok = false;
            message = "The device is mounted but has no local filesystem path. Enable the GVfs FUSE bridge to browse it.";
        }
    }

    pending.clear();
    action.clear();
    g_clear_object(&cancel);

    JsonValue result;
    result["finished"] = JsonValue(true);
    result["ok"] = JsonValue(ok);
    result["id"] = JsonValue(id);
    result["action"] = JsonValue(verb);
    result["path"] = JsonValue(path);
    result["reason"] = JsonValue(ok || !message.empty() ? message : "The device operation failed without an error reason.");

    send(result);
}

static void abortInteraction(GMountOperation *operation, const char *message, gpointer data) {
    auto *monitor = static_cast<Monitor *>(data);
    monitor->interactionReason = (message ? message : "");
    if (monitor->interactionReason.empty())
        monitor->interactionReason = "This device requires an interactive response. Unlock it with the system device manager and try again.";
    g_mount_operation_reply(operation, G_MOUNT_OPERATION_ABORTED);
}

static void askPassword(GMountOperation *operation, const char *message, const char *, const char *, GAskPasswordFlags, gpointer data) {
    abortInteraction(operation, message, data);
}

static void askQuestion(GMountOperation *operation, const char *message, const char **, gpointer data) {
    abortInteraction(operation, message, data);
}

void Monitor::operate(const std::string &id, const std::string &verb) {
    if (verb == "cancel") {
        if (cancel) g_cancellable_cancel(cancel);
        return;
    }
    if (!pending.empty()) return;

    pending = id;
    action = verb;
    interactionReason.clear();

    auto it = devices.find(id);
    if (it == devices.end()) {
        finish(false, "The device is no longer connected.");
        return;
    }

    auto item = it->second;
    cancel = g_cancellable_new();
    GMountOperation *operation = g_mount_operation_new();
    g_signal_connect(operation, "ask-password", G_CALLBACK(askPassword), this);
    g_signal_connect(operation, "ask-question", G_CALLBACK(askQuestion), this);
    g_signal_connect(operation, "show-processes", G_CALLBACK(+[](GMountOperation *op, const char *message, GArray *, const char **, gpointer data) {
        abortInteraction(op, message, data);
    }), this);

    struct Completion {
        Monitor *monitor;
        std::shared_ptr<Device> device;
        std::string kind;
    };
    auto *completion = new Completion{this, item, {}};

    const GAsyncReadyCallback callback = +[](GObject *source, GAsyncResult *result, gpointer data) {
        std::unique_ptr<Completion> completion(static_cast<Completion *>(data));
        GError *error = nullptr;
        bool ok = false;

        if (completion->kind == "mount") {
            ok = g_volume_mount_finish(G_VOLUME(source), result, &error);
        } else if (completion->kind == "unmount") {
            ok = g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error);
        } else if (completion->kind == "volume-eject") {
            ok = g_volume_eject_with_operation_finish(G_VOLUME(source), result, &error);
        } else if (completion->kind == "drive-eject") {
            ok = g_drive_eject_with_operation_finish(G_DRIVE(source), result, &error);
        } else if (completion->kind == "stop") {
            ok = g_drive_stop_finish(G_DRIVE(source), result, &error);
        } else if (completion->kind == "mount-eject") {
            ok = g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error);
        }

        std::string reason = error ? std::string(error->message) : "";
        if (error && error->domain == G_IO_ERROR && error->code == G_IO_ERROR_FAILED_HANDLED) {
            reason = completion->monitor->interactionReason.empty()
                ? "The device operation was canceled or requires an interactive response."
                : completion->monitor->interactionReason;
        }
        g_clear_error(&error);
        completion->monitor->finish(ok, reason);
    };

    if (verb == "open" && item->mount) {
        delete completion;
        finish(true, {});
    } else if ((verb == "open" || verb == "mount") && g_volume_can_mount(item->volume)) {
        completion->kind = "mount";
        g_volume_mount(item->volume, G_MOUNT_MOUNT_NONE, operation, cancel, callback, completion);
    } else if (verb == "unmount" && item->mount && g_mount_can_unmount(item->mount)) {
        completion->kind = "unmount";
        g_mount_unmount_with_operation(item->mount, G_MOUNT_UNMOUNT_NONE, operation, cancel, callback, completion);
    } else if (verb == "eject" && item->drive && g_drive_can_stop(item->drive)) {
        completion->kind = "stop";
        g_drive_stop(item->drive, G_MOUNT_UNMOUNT_NONE, operation, cancel, callback, completion);
    } else if (verb == "eject" && g_volume_can_eject(item->volume)) {
        completion->kind = "volume-eject";
        g_volume_eject_with_operation(item->volume, G_MOUNT_UNMOUNT_NONE, operation, cancel, callback, completion);
    } else if (verb == "eject" && item->drive && g_drive_can_eject(item->drive)) {
        completion->kind = "drive-eject";
        g_drive_eject_with_operation(item->drive, G_MOUNT_UNMOUNT_NONE, operation, cancel, callback, completion);
    } else if (verb == "eject" && item->mount && g_mount_can_eject(item->mount)) {
        completion->kind = "mount-eject";
        g_mount_eject_with_operation(item->mount, G_MOUNT_UNMOUNT_NONE, operation, cancel, callback, completion);
    } else {
        delete completion;
        finish(false, "This device does not support the requested operation.");
    }

    g_object_unref(operation);
}
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Monitor monitor;
    monitor.loop = g_main_loop_new(nullptr, false);
    monitor.volumes = g_volume_monitor_get();

    for (const char *signal : {"volume-added", "volume-removed", "volume-changed", "mount-added", "mount-removed", "mount-changed", "drive-connected", "drive-disconnected", "drive-changed"})
        g_signal_connect(monitor.volumes, signal, G_CALLBACK(changed), &monitor);

    monitor.snapshot();

    g_unix_fd_add(STDIN_FILENO, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), +[](gint fd, GIOCondition condition, gpointer data) -> gboolean {
        auto *monitor = static_cast<Monitor *>(data);
        char buffer[65536];
        ssize_t count = ::read(fd, buffer, sizeof(buffer));

        if (count <= 0 || (condition & G_IO_ERR)) {
            g_main_loop_quit(monitor->loop);
            return G_SOURCE_REMOVE;
        }

        monitor->input.append(buffer, count);
        if (monitor->input.size() > 1024 * 1024) {
            monitor->exitCode = 2;
            g_main_loop_quit(monitor->loop);
            return G_SOURCE_REMOVE;
        }

        for (;;) {
            size_t end = monitor->input.find('\n');
            if (end == std::string::npos) break;

            std::string line = monitor->input.substr(0, end);
            JsonParser parser(line);
            JsonValue document = parser.parse();
            const JsonValue &request = document;

            std::string action = request.value("action").toString();

            bool valid = request.isObject() && !action.empty()
                && (action == "refresh" || action == "cancel" || action == "open" || action == "mount" || action == "unmount" || action == "eject");

            if (valid && action != "refresh" && action != "cancel") {
                std::string id = request.value("id").toString();
                if (id.empty() || id.find('\0') != std::string::npos) valid = false;
            }

            monitor->input.erase(0, end + 1);

            if (!valid) {
                monitor->exitCode = 2;
                g_main_loop_quit(monitor->loop);
                return G_SOURCE_REMOVE;
            }

            if (action == "refresh") {
                monitor->snapshot();
            } else {
                monitor->operate(request.value("id").toString(), action);
            }
        }

        return G_SOURCE_CONTINUE;
    }, &monitor);

    g_main_loop_run(monitor.loop);

    if (monitor.cancel) g_cancellable_cancel(monitor.cancel);
    g_clear_object(&monitor.cancel);
    g_object_unref(monitor.volumes);
    g_main_loop_unref(monitor.loop);

    return monitor.exitCode;
}
