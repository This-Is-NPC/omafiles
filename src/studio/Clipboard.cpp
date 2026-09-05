#include "Clipboard.h"

#include <QClipboard>
#include <QGuiApplication>

namespace omafiles {

Clipboard::Clipboard(QObject *parent)
    : QObject(parent)
{
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

} // namespace omafiles
