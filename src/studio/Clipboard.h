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
    /// Whether what is on the clipboard is files -- put there by this window or
    /// by any other file manager -- so a `Paste` entry can be drawn only when
    /// there is something to paste.
    Q_PROPERTY(bool holdsFiles READ holdsFiles NOTIFY changed)

public:
    explicit Clipboard(QObject *parent = nullptr);

    QString lastCopied() const { return m_lastCopied; }
    bool holdsFiles() const { return !files().isEmpty(); }

    Q_INVOKABLE void copy(const QString &text);

    /// Files, the way the desktop's file managers pass them: `text/uri-list`
    /// for anyone, and the GNOME and KDE notes beside it that say whether
    /// they were cut. So a Ctrl+V in Nautilus or Dolphin takes what was
    /// copied here, and a Ctrl+V here takes what was copied there.
    Q_INVOKABLE void copyFiles(const QStringList &paths, bool cut);
    /// The local files on the clipboard, wherever they were put there.
    Q_INVOKABLE QStringList files() const;
    /// Whether they were cut rather than copied, by either desktop's spelling.
    Q_INVOKABLE bool cutting() const;
    /// Empties it. A cut that has been pasted has been moved, and a second
    /// paste would be a `mv` of files that are no longer where the list said.
    Q_INVOKABLE void clear();

signals:
    void copied(const QString &text);
    void changed();

private:
    QString m_lastCopied;
};

} // namespace omafiles
