#include "Clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
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

// The text form goes with the files, one path per line, so a paste into a
// terminal or an editor still gets something it can use: the desktop offers
// a paste target whichever format it can take, and a list of paths is the
// text a list of files is.
void Clipboard::copyFiles(const QStringList &paths, bool cut)
{
    if (paths.isEmpty())
        return;
    QList<QUrl> urls;
    QStringList lines;
    for (const QString &path : paths) {
        urls.append(QUrl::fromLocalFile(path));
        lines.append(urls.last().toString(QUrl::FullyEncoded));
    }
    const QString text = paths.join(QLatin1Char('\n'));
    if (QClipboard *board = QGuiApplication::clipboard()) {
        auto *data = new QMimeData;
        data->setUrls(urls);
        data->setData(QLatin1String(kGnomeFiles),
                      (QLatin1String(cut ? "cut" : "copy") + QLatin1Char('\n')
                       + lines.join(QLatin1Char('\n'))).toUtf8());
        if (cut)
            data->setData(QLatin1String(kKdeCut), QByteArrayLiteral("1"));
        data->setText(text);
        board->setMimeData(data, QClipboard::Clipboard);
        if (board->supportsSelection())
            board->setText(text, QClipboard::Selection);
    }
    m_lastCopied = text;
    emit copied(text);
}

QStringList Clipboard::files() const
{
    QStringList paths;
    const QClipboard *board = QGuiApplication::clipboard();
    const QMimeData *data = board ? board->mimeData(QClipboard::Clipboard) : nullptr;
    if (!data || !data->hasUrls())
        return paths;
    for (const QUrl &url : data->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    return paths;
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
