#pragma once

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

namespace omafiles {

/// The clipboard, as one call.
///
/// A singleton rather than a call in QML because QML has no clipboard: every
/// Qt Quick program that copies text reaches into `QGuiApplication` for it, and
/// doing that from three menu entries is three places to get the mode wrong.
class Clipboard : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// What was last put on the clipboard by this window.
    ///
    /// Not a cache -- nothing reads it back to paste. It is here because the
    /// clipboard belongs to the desktop and a test has no desktop to ask: on a
    /// headless run `QClipboard` keeps nothing, so without this the only way to
    /// check that `Copy path` copied the path would be to not check it.
    Q_PROPERTY(QString lastCopied READ lastCopied NOTIFY copied)

public:
    explicit Clipboard(QObject *parent = nullptr);

    QString lastCopied() const { return m_lastCopied; }

    Q_INVOKABLE void copy(const QString &text);

signals:
    void copied(const QString &text);

private:
    QString m_lastCopied;
};

} // namespace omafiles
