#include "Clipboard.h"

#include "Paths.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QSet>
#include <QUrl>

namespace omafiles {

namespace {

const char *const kGnomeFiles = "x-special/gnome-copied-files";
const char *const kKdeCut = "application/x-kde-cutselection";

} // namespace

Clipboard::Clipboard(QObject *parent)
    : QObject(parent)
{
    // Whatever changes it, this window or another program: `holdsFiles` is a
    // question about the desktop's clipboard, and the answer changes when the
    // desktop does.
    if (QClipboard *board = QGuiApplication::clipboard())
        connect(board, &QClipboard::dataChanged, this, &Clipboard::changed);
}

// Both selections, because a Linux desktop has two and a person who copies a
// path means to paste it wherever they paste: the clipboard for a Ctrl+V into a
// dialog, the primary selection for a middle click into a terminal.
void Clipboard::copy(const QString &text)
{
    if (QClipboard *board = QGuiApplication::clipboard()) {
        board->setText(text, QClipboard::Clipboard);
        if (board->supportsSelection())
            board->setText(text, QClipboard::Selection);
    }
    m_lastCopied = text;
    emit copied(text);
}

QStringList localFilePaths(const QList<QUrl> &urls)
{
    QStringList paths;
    QSet<QString> seen;
    for (const QUrl &url : urls) {
        const QString path = localFileOf(url);
        if (path.isEmpty() || !path.startsWith(QLatin1Char('/')))
            return {};
        if (!seen.contains(path)) {
            seen.insert(path);
            paths.append(path);
        }
    }
    return paths;
}

// Offer paths as text alongside the native desktop file formats.
QMimeData *fileMimeData(const QStringList &paths, bool cut)
{
    QList<QUrl> urls;
    QStringList lines;
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
        lines.append(urls.last().toString(QUrl::FullyEncoded));
    }
    auto *data = new QMimeData;
    data->setUrls(urls);
    data->setData(QLatin1String(kGnomeFiles),
                  (QLatin1String(cut ? "cut" : "copy") + QLatin1Char('\n')
                   + lines.join(QLatin1Char('\n'))).toUtf8());
    if (cut)
        data->setData(QLatin1String(kKdeCut), QByteArrayLiteral("1"));
    data->setText(paths.join(QLatin1Char('\n')));
    return data;
}

void Clipboard::copyFiles(const QStringList &paths, bool cut)
{
    if (paths.isEmpty())
        return;
    const QString text = paths.join(QLatin1Char('\n'));
    if (QClipboard *board = QGuiApplication::clipboard()) {
        auto *data = fileMimeData(paths, cut);
        board->setMimeData(data, QClipboard::Clipboard);
        if (board->supportsSelection())
            board->setText(text, QClipboard::Selection);
    }
    m_lastCopied = text;
    emit copied(text);
}

QStringList Clipboard::files() const
{
    const QClipboard *board = QGuiApplication::clipboard();
    const QMimeData *data = board ? board->mimeData(QClipboard::Clipboard) : nullptr;
    return data && data->hasUrls() ? localFilePaths(data->urls()) : QStringList{};
}

bool Clipboard::cutting() const
{
    const QClipboard *board = QGuiApplication::clipboard();
    const QMimeData *data = board ? board->mimeData(QClipboard::Clipboard) : nullptr;
    if (!data)
        return false;
    if (data->hasFormat(QLatin1String(kKdeCut)))
        return data->data(QLatin1String(kKdeCut)).trimmed() == "1";
    if (data->hasFormat(QLatin1String(kGnomeFiles)))
        return data->data(QLatin1String(kGnomeFiles)).startsWith("cut");
    return false;
}

void Clipboard::clear()
{
    if (QClipboard *board = QGuiApplication::clipboard())
        board->clear(QClipboard::Clipboard);
}

} // namespace omafiles
