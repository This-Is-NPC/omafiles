#include "FileTransfer.h"
#include "BlockingJob.h"

#include "Clipboard.h"
#include "Count.h"
#include "Paths.h"
#include "Theme.h"
#include "Thumbnail.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QQmlEngine>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QPainter>
#include <QFutureWatcher>
#include <QQuickItem>
#include <QQuickWindow>
#include <QPointer>
#include <sys/stat.h>
#include <cerrno>
#include <utility>

namespace omafiles {
namespace {
FileDragRunner dragRunner;

struct DropPlan {
    Qt::DropAction action = Qt::IgnoreAction;
    QString error;
};

DropPlan planDrop(const QStringList &paths, const QString &directory,
                  Qt::KeyboardModifiers modifiers, Qt::DropActions possible)
{
    struct stat destination {};
    if (::stat(QFile::encodeName(directory).constData(), &destination) != 0)
        return {Qt::IgnoreAction, QStringLiteral("Cannot drop into %1: %2")
                    .arg(directory, systemReason(errno))};
    if (!S_ISDIR(destination.st_mode))
        return {Qt::IgnoreAction, QStringLiteral("Cannot drop: %1 is not a folder").arg(directory)};
    bool same = true;
    for (const QString &path : paths) {
        struct stat source {};
        if (::lstat(QFile::encodeName(path).constData(), &source) != 0)
            return {Qt::IgnoreAction, QStringLiteral("Cannot drop %1: %2")
                        .arg(path, systemReason(errno))};
        same &= source.st_dev == destination.st_dev;
    }
    return {fileDropAction(same, modifiers, possible), {}};
}

// The drag image's card, the step between it and each card behind it, and the
// tag squares, in logical pixels.
constexpr int cardWidth = 160;
constexpr int cardHeight = 64;
constexpr int stackStep = Theme::spacingXs();
constexpr int tagSquare = Theme::spacingS();
constexpr int shownTags = 3;

// Paints the row's icon as the list shows it into `slot`: its thumbnail when
// one is shown, otherwise the glyph its canvas drew.
void paintIcon(QPainter &painter, QQuickItem *icon, const QRectF &slot)
{
    if (!icon || icon->width() <= 0 || icon->height() <= 0)
        return;
    auto *thumbnail = qobject_cast<Thumbnail *>(icon->property("thumbnail").value<QObject *>());
    if (thumbnail && thumbnail->ready() && thumbnail->width() > 0 && thumbnail->height() > 0) {
        painter.save();
        painter.translate(slot.topLeft());
        painter.scale(slot.width() / thumbnail->width(), slot.height() / thumbnail->height());
        thumbnail->paint(&painter);
        painter.restore();
        return;
    }
    for (QQuickItem *child : icon->childItems()) {
        if (!child->inherits("QQuickCanvasItem"))
            continue;
        QString url;
        QMetaObject::invokeMethod(child, "toDataURL", Q_RETURN_ARG(QString, url),
                                  Q_ARG(QString, QStringLiteral("image/png")));
        const QImage glyph = QImage::fromData(QByteArray::fromBase64(url.section(QLatin1Char(','), 1).toLatin1()));
        painter.drawImage(slot, glyph);
        return;
    }
}

// One opaque raised card with a line border: the drag image, or a card behind it.
void paintCard(QPainter &painter, const Theme &theme, const QRectF &card)
{
    painter.setPen(QPen(theme.line(), 1));
    painter.setBrush(theme.raised());
    painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), theme.rounding(), theme.rounding());
}

// The card of the row under the pointer: icon and name, a rule, up to three tag
// colours and the tag count. With more paths, two cards behind it and a badge
// with the count at its top right.
QPixmap dragImage(const Theme &theme, QQuickItem *icon, const QString &shown,
                  const QStringList &tags, qsizetype count, qreal ratio)
{
    QFont caption(theme.fontFamily());
    caption.setPixelSize(Theme::fontCaption());
    QFont badgeFont = caption;
    badgeFont.setWeight(QFont::DemiBold);
    const QString number = QString::number(count);
    const int badgeHeight = Theme::controlHeight();
    const int badgeWidth = qMax(badgeHeight, QFontMetrics(badgeFont).horizontalAdvance(number)
                                                + 2 * Theme::spacingS());
    const bool many = count > 1;
    const int top = many ? qMax(2 * stackStep, badgeHeight / 2) : 0;
    const int right = many ? qMax(2 * stackStep, (badgeWidth + 1) / 2) : 0;

    QPixmap pixmap(QSize(cardWidth + right, cardHeight + top) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF card(0, top, cardWidth, cardHeight);
    if (many) {
        for (int step = 2; step > 0; --step)
            paintCard(painter, theme, card.translated(step * stackStep, -step * stackStep));
    }
    paintCard(painter, theme, card);

    const int inset = Theme::inset();
    const qreal middle = card.top() + cardHeight / 2.0;
    const QRectF slot(inset, middle - Theme::spacingXs() - Theme::iconSize(),
                      Theme::iconSize(), Theme::iconSize());
    paintIcon(painter, icon, slot);

    QFont body(theme.fontFamily());
    body.setPixelSize(Theme::fontBody());
    painter.setFont(body);
    painter.setPen(theme.foreground());
    const QRectF nameBox(slot.right() + Theme::iconGap(), slot.top(),
                         cardWidth - inset - slot.right() - Theme::iconGap(), slot.height());
    painter.drawText(nameBox, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(body).elidedText(QFileInfo(shown).fileName(), Qt::ElideMiddle,
                                                   int(nameBox.width())));

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.line());
    painter.drawRect(QRectF(inset, middle, cardWidth - 2 * inset, 1));

    const qreal tagRow = middle + Theme::spacingS();
    qreal x = inset;
    for (const QString &tag : tags.mid(0, shownTags)) {
        painter.setBrush(theme.tagColour(tag));
        painter.drawRect(QRectF(x, tagRow, tagSquare, tagSquare));
        x += tagSquare + Theme::spacingXs();
    }
    painter.setFont(caption);
    painter.setPen(theme.dim());
    painter.drawText(QRectF(x, tagRow, cardWidth - inset - x, tagSquare),
                     Qt::AlignLeft | Qt::AlignVCenter, countOf(tags.size(), QStringLiteral("tag")));

    if (many) {
        const QRectF badge(cardWidth - badgeWidth / 2.0, top - badgeHeight / 2.0, badgeWidth, badgeHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.accent());
        painter.drawRoundedRect(badge, badgeHeight / 2.0, badgeHeight / 2.0);
        painter.setFont(badgeFont);
        painter.setPen(theme.base());
        painter.drawText(badge, Qt::AlignCenter, number);
    }
    return pixmap;
}
} // namespace

FileDragRunner setFileDragRunner(FileDragRunner runner)
{
    return std::exchange(dragRunner, std::move(runner));
}

// An action the source does not offer becomes a copy, as QDropEvent::setDropAction does.
Qt::DropAction fileDropAction(bool sameDevice, Qt::KeyboardModifiers modifiers,
                              Qt::DropActions possible)
{
    const bool copy = modifiers.testFlag(Qt::ControlModifier);
    const bool move = modifiers.testFlag(Qt::ShiftModifier);
    const Qt::DropAction action = copy && move ? Qt::LinkAction
        : copy ? Qt::CopyAction
        : move || sameDevice ? Qt::MoveAction
        : Qt::CopyAction;
    return possible.testFlag(action) ? action : Qt::CopyAction;
}

void FileTransfer::startDrag(QQuickItem *source, const QStringList &paths, QQuickItem *icon,
                             const QString &shown, const QStringList &tags)
{
    if (!source || !source->window() || paths.isEmpty())
        return;
    // A delegate can disappear during the native drag's nested event loop.
    QPointer<QDrag> drag = new QDrag(source->window());
    drag->setMimeData(fileMimeData(paths, false));
    if (auto *engine = qmlEngine(source)) {
        if (auto *theme = engine->singletonInstance<Theme *>(QStringLiteral("omafiles"),
                                                             QStringLiteral("Theme"))) {
            drag->setPixmap(dragImage(*theme, icon, shown, tags, paths.size(),
                                      source->window()->effectiveDevicePixelRatio()));
            drag->setHotSpot(QPoint(Theme::spacingS(), Theme::spacingS()));
        }
    }
    const auto runner = dragRunner;
    if (runner)
        runner(drag);
    else
        drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::CopyAction);
    if (drag)
        drag->deleteLater();
}

bool FileTransfer::receive(const QList<QUrl> &urls, const QString &directory,
                            int modifiers, int possibleActions)
{
    // Copy transport prevents a foreign source deleting files before our
    // asynchronous operation succeeds. The destination owns the whole move.
    if (!(possibleActions & Qt::CopyAction)) {
        emit refused(QStringLiteral("Cannot drop: the source must offer file copying"));
        return false;
    }
    if (urls.isEmpty() || directory.isEmpty())
        return false;
    const QStringList paths = localFilePaths(urls);
    if (paths.isEmpty()) {
        emit refused(QStringLiteral("Cannot drop: every URL must name a local file"));
        return false;
    }
    auto *watcher = new QFutureWatcher<DropPlan>(this);
    connect(watcher, &QFutureWatcher<DropPlan>::finished, this,
            [this, watcher, paths, directory] {
        const auto plan = watcher->result();
        watcher->deleteLater();
        if (!plan.error.isEmpty())
            emit refused(plan.error);
        else
            emit requested(paths, directory, plan.action);
    });
    watcher->setFuture(runBlocking([paths, directory, modifiers, possibleActions] {
        return planDrop(paths, directory, Qt::KeyboardModifiers(modifiers),
                        Qt::DropActions(possibleActions));
    }));
    return true;
}

FileDropTarget::FileDropTarget(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemAcceptsDrops, true);
}

void FileDropTarget::setDirectory(const QString &directory)
{
    if (m_directory == directory)
        return;
    m_directory = directory;
    emit directoryChanged();
}

void FileDropTarget::setContainsDrag(bool contains)
{
    if (m_containsDrag == contains)
        return;
    m_containsDrag = contains;
    emit containsDragChanged();
}

void FileDropTarget::dragEnterEvent(QDragEnterEvent *event)
{
    dragMoveEvent(event);
}

void FileDropTarget::dragMoveEvent(QDragMoveEvent *event)
{
    const bool accepted = !m_directory.isEmpty() && isEnabled()
        && event->mimeData()->hasUrls() && event->possibleActions().testFlag(Qt::CopyAction);
    setContainsDrag(accepted);
    event->setDropAction(Qt::CopyAction);
    event->setAccepted(accepted);
}

void FileDropTarget::dragLeaveEvent(QDragLeaveEvent *)
{
    setContainsDrag(false);
}

void FileDropTarget::dropEvent(QDropEvent *event)
{
    setContainsDrag(false);
    event->ignore();
    auto *engine = qmlEngine(this);
    if (!engine || m_directory.isEmpty() || !isEnabled())
        return;
    auto *transfer = engine->singletonInstance<FileTransfer *>(QStringLiteral("omafiles"),
                                                               QStringLiteral("FileTransfer"));
    if (transfer && transfer->receive(event->mimeData()->urls(), m_directory,
                                     event->modifiers(), event->possibleActions())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

} // namespace omafiles
