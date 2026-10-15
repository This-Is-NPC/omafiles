#include "Preview.h"
#include "HelperProcess.h"
#include "../preview/Protocol.h"
#include <QCoreApplication>
#include <QDir>
#include <QPainter>
#include <QtEndian>
#include <utility>

namespace omafiles {
namespace { QString helperProgram; }
QString setPreviewHelper(QString program) { return std::exchange(helperProgram, std::move(program)); }

Preview::Preview(QQuickItem *parent) : QQuickPaintedItem(parent), m_process(new QProcess(this))
{
    prepareHelper(*m_process, 0, 0);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    environment.remove("DBUS_SESSION_BUS_ADDRESS");
    m_process->setProcessEnvironment(environment);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &Preview::receive);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!m_pending && error == QProcess::FailedToStart)
            fail("Preview helper could not start: " + m_process->errorString());
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        receive();
        m_monitor.stop();
        if (m_pending) start();
        else if (!m_received && !m_path.isEmpty()) fail("Preview could not be read.");
        else if (!m_failed && (status == QProcess::CrashExit || (code != 0 && m_kind != "error") || m_playing))
            fail("Preview helper stopped unexpectedly. Close and retry, or open the file normally.");
    });
    m_monitor.setInterval(25);
    connect(&m_monitor, &QTimer::timeout, this, [this] {
        if (helperResidentBytes(m_process->processId()) > helperMemoryLimit)
            fail("Preview stopped: its 512 MiB memory budget was exceeded.");
        else if (m_loading && m_elapsed.elapsed() > 5000)
            fail("Preview timed out after five seconds. Try again or open the file normally.");
    });
}

Preview::~Preview()
{
    m_process->disconnect(this);
    m_process->setParent(nullptr);
    if (m_process->state() == QProcess::NotRunning) delete m_process;
    else {
        connect(m_process, &QProcess::finished, m_process, &QObject::deleteLater);
        killHelper(*m_process);
    }
}

void Preview::setPath(const QString &path)
{
    if (m_path == path) return;
    m_path = path;
    m_pending = true;
    m_kind.clear(); m_text.clear(); m_image = {};
    m_loading = !path.isEmpty(); m_playing = false;
    update(); emit pathChanged(); emit changed();
    if (m_process->state() != QProcess::NotRunning) killHelper(*m_process);
    else start();
}

void Preview::start()
{
    m_pending = false; m_received = false; m_failed = false; m_bytes.clear();
    if (m_path.isEmpty()) return;
    m_elapsed.start();
    const QString program = helperProgram.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath("omafiles-preview") : helperProgram;
    m_process->start(program, {m_path});
    m_monitor.start();
}

void Preview::fail(const QString &message)
{
    m_kind = "error"; m_text = message; m_loading = false; m_playing = false;
    m_received = true; m_failed = true; m_image = {}; m_monitor.stop();
    killHelper(*m_process);
    update(); emit changed();
}

void Preview::receive()
{
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (m_pending || m_failed) return;
    m_bytes += bytes;
    while (m_bytes.size() >= 4) {
        const quint32 size = qFromBigEndian<quint32>(m_bytes.constData());
        if (size > previewPacketLimit || m_bytes.size() > 2 * previewPacketLimit) {
            fail("Preview returned too much data."); m_bytes.clear(); return;
        }
        if (m_bytes.size() < size + 4) return;
        PreviewPacket packet;
        if (!readPreviewPacket(m_bytes.mid(4, size), &packet)) {
            fail("Preview returned invalid data."); m_bytes.clear(); return;
        }
        m_bytes.remove(0, size + 4);
        m_kind = packet.kind; m_text = packet.text; m_playing = packet.playing;
        if (packet.loading && !m_loading) m_elapsed.start();
        m_loading = packet.loading; m_received = true;
        if (!packet.image.isNull()) m_image = packet.image;
        update(); emit changed();
    }
}

void Preview::togglePlayback()
{
    if (m_kind == "audio" || m_kind == "video") m_process->write("toggle\n");
}
void Preview::pause()
{
    if (m_kind == "audio" || m_kind == "video") m_process->write("pause\n");
}
void Preview::seek(int seconds)
{
    if (m_kind == "audio" || m_kind == "video")
        m_process->write("seek " + QByteArray::number(qBound(-30, seconds, 30)) + '\n');
}
void Preview::paint(QPainter *painter)
{
    if (m_image.isNull()) return;
    const QSizeF size = m_image.size().scaled(boundingRect().size().toSize(), Qt::KeepAspectRatio);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(QRectF((width() - size.width()) / 2, (height() - size.height()) / 2,
                             size.width(), size.height()), m_image);
}
}
