#include "Devices.h"
#include "Paths.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace omafiles {
Devices::Devices(QObject *parent) : QObject(parent), m_process(new QProcess(this)) {
    connect(m_process, &QProcess::readyReadStandardOutput, this, &Devices::receive);
    connect(m_process, &QProcess::readyReadStandardError, this, [this] { m_process->readAllStandardError(); });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) fail("Device helper could not start: " + m_process->errorString());
    });
    connect(m_process, &QProcess::finished, this, [this] {
        receive();
        if (!m_failed) fail("Device monitor stopped. Reopen the window to reconnect.");
    });
}
Devices::~Devices() {
    m_process->disconnect(this);
    m_process->setParent(nullptr);
    if (m_process->state() == QProcess::NotRunning) delete m_process;
    else {
        connect(m_process, &QProcess::finished, m_process, &QObject::deleteLater);
        m_process->kill();
    }
}
void Devices::start(const QString &program) {
    if (m_process->state() != QProcess::NotRunning) return;
    m_failed = false; m_input.clear(); m_status = "Discovering devices"; emit changed();
    const QString helper = program.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("omafiles-devices") : program;
    m_process->start(helper, {});
}
void Devices::fail(const QString &reason) {
    m_failed = true;
    const bool hadEntries = !m_entries.isEmpty();
    m_entries.clear(); m_busy = false; m_pending.clear(); m_action.clear();
    m_status = reason; emit changed();
    if (hadEntries) emit storageChanged();
    emit notice(reason);
}
void Devices::receive() {
    if (m_failed) { m_process->readAllStandardOutput(); return; }
    int messages = 0;
    for (;;) {
        if (++messages > 32) {
            if (!m_input.isEmpty() || m_process->bytesAvailable())
                QMetaObject::invokeMethod(this, [this] { receive(); }, Qt::QueuedConnection);
            return;
        }
        auto end = m_input.indexOf('\n');
        if (end < 0) {
            m_input += m_process->read(65536);
            end = m_input.indexOf('\n');
        }
        if (end > 1024 * 1024 || (end < 0 && m_input.size() > 1024 * 1024)) {
            fail("Device monitor returned too much data."); m_process->kill(); return;
        }
        if (end < 0) {
            if (m_process->bytesAvailable()) continue;
            break;
        }
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(m_input.left(end), &error);
        m_input.remove(0, end + 1);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail("Device monitor returned invalid data."); m_process->kill(); return;
        }
        const auto object = document.object();
        if (object.contains("devices")) {
            if (!object.value("devices").isArray()) {
                fail("Device monitor returned invalid data."); m_process->kill(); return;
            }
            QVariantList entries;
            QSet<QString> seen;
            for (const auto &value : object.value("devices").toArray()) {
                const auto row = value.toObject();
                const QString id = row.value("id").toString(), path = row.value("path").toString();
                bool flagsValid = true;
                for (const char *flag : {"phone", "mounted", "canMount", "canUnmount", "canEject"})
                    flagsValid &= row.value(QLatin1String(flag)).isBool();
                if (!flagsValid || !row.value("path").isString() || id.isEmpty() || seen.contains(id) || id.contains(QChar(0))
                    || row.value("name").toString().isEmpty() || row.value("name").toString().contains(QChar(0)) || (path.contains(QChar(0)) || (!path.isEmpty() && !isCleanAbsolute(path)))) {
                    fail("Device monitor returned an invalid device path or identity."); m_process->kill(); return;
                }
                seen.insert(id); entries.append(row.toVariantMap());
            }
            const bool different = entries != m_entries;
            m_entries = entries;
            if (!m_busy) m_status = entries.isEmpty() ? "No removable devices" : QString();
            emit changed();
            if (different) emit storageChanged();
        }
        if (!object.contains("devices") && !object.value("finished").isBool()) {
            fail("Device monitor returned invalid data."); m_process->kill(); return;
        }
        if (object.value("finished").toBool() && m_busy && object.value("id").toString() == m_pending
            && object.value("action").toString() == m_action) {
            if (!object.value("ok").isBool()) {
                fail("Device monitor returned invalid data."); m_process->kill(); return;
            }
            const bool ok = object.value("ok").toBool();
            const QString path = object.value("path").toString();
            const QString action = m_action;
            m_busy = false; m_pending.clear(); m_action.clear();
            m_status = ok ? "Device operation completed" : object.value("reason").toString();
            if (m_status.isEmpty()) m_status = "Device operation failed without an error reason.";
            emit changed();
            emit storageChanged();
            if (ok && action == "open") {
                if (!path.contains(QChar(0)) && isCleanAbsolute(path)) emit openRequested(path);
                else { fail("Device monitor returned an invalid mount path."); m_process->kill(); return; }
            }
            emit notice(m_status);
        }
    }
}
void Devices::act(const QString &id, const QString &action) {
    if (action == "cancel" && !m_busy) return;
    if (action != "cancel" && m_busy) return;
    if (action != "open" && action != "mount" && action != "unmount" && action != "eject" && action != "cancel") return;
    if (m_process->state() != QProcess::Running) { fail("Device monitor is unavailable. Reopen the window to reconnect."); return; }
    if (action != "cancel") {
        bool present = false;
        for (const auto &row : m_entries) present |= row.toMap().value("id").toString() == id;
        if (!present) { m_status = "The device is no longer connected."; emit changed(); emit notice(m_status); return; }
        m_busy = true; m_pending = id; m_action = action;
    }
    m_status = action == "cancel" ? "Canceling device operation" : "Device operation in progress";
    emit notice(QString());
    emit changed();
    m_process->write(QJsonDocument(QJsonObject{{"id", id}, {"action", action}}).toJson(QJsonDocument::Compact) + '\n');
}
void Devices::refresh() {
    if (m_process->state() == QProcess::Running && m_process->bytesToWrite() < 4096)
        m_process->write("{\"action\":\"refresh\"}\n");
}
}
