#include "GraphItem.h"

#include "IndexModel.h"
#include "Paths.h"
#include "Record.h"

#include <QFileInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <climits>

namespace omafiles {
GraphItem::GraphItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setAntialiasing(true);
    setOpaquePainting(true);
}

void GraphItem::setTheme(Theme *theme)
{
    if (m_theme == theme)
        return;
    if (m_theme)
        disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme)
        connect(m_theme, &Theme::changed, this, [this] { update(); });
    emit themeChanged();
    update();
}

void GraphItem::setIndex(IndexModel *index)
{
    if (m_index == index)
        return;
    if (m_index)
        disconnect(m_index, nullptr, this, nullptr);
    m_index = index;
    if (m_index)
        connect(m_index, &IndexModel::changed, this, [this] {
            m_dirty = true;
            ensureNodes();
            update();
        });
    m_dirty = true;
    ensureNodes();
    emit indexChanged();
    update();
}

void GraphItem::setSelectedId(const QString &id)
{
    if (m_selectedId == id)
        return;
    m_selectedId = id;
    emit selectedIdChanged();
    emit graphChanged();
    update();
}

void GraphItem::resetView()
{
    m_scale = 1.0;
    m_pan = QPointF();
    emit viewChanged();
    update();
}

void GraphItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    // A window of another size holds the same map: the nodes stand, only where
    // they sit is recomputed.
    if (newGeometry.size() != oldGeometry.size()) {
        ensureNodes();
        layout();
        update();
    }
}

// The node list is state, not something a repaint produces: built only inside
// paint(), a key pressed before the first frame finds an empty graph and the
// selection has nowhere to go.
void GraphItem::ensureNodes()
{
    if (!m_dirty)
        return;
    rebuild();
    m_dirty = false;
    emit graphChanged();
}

void GraphItem::rebuild()
{
    m_nodes.clear();
    m_edges.clear();
    m_tagCount = 0;
    m_taggedFiles = 0;
    if (!m_index)
        return;

    QHash<QString, int> tagIndex;
    QHash<QString, int> tagDegree;

    QVector<Record> live;
    const QVector<Record> records = m_index->store().records();
    for (const Record &record : records) {
        if (rowState(record.path, nullptr) != PathState::Present)
            continue;
        live.append(record);
        for (const QString &tag : record.tags)
            tagDegree[tag] += 1;
    }

    // Tags in a stable order, so the map does not rearrange itself between two
    // reloads of the same index.
    QStringList tagNames = tagDegree.keys();
    std::sort(tagNames.begin(), tagNames.end());
    for (const QString &tag : std::as_const(tagNames)) {
        Node node;
        node.id = QStringLiteral("tag:") + tag;
        node.kind = QStringLiteral("tag");
        node.label = tag;
        tagIndex.insert(tag, m_nodes.size());
        m_nodes.append(node);
    }
    m_tagCount = m_nodes.size();

    // Past this many names the rings run into each other and the map stops
    // being one. What it leaves out is counted below and said on the status
    // line, so the picture is never mistaken for the whole index.
    constexpr int kMaxFiles = 300;
    m_taggedFiles = 0;
    for (const Record &record : std::as_const(live)) {
        // A file hangs off one tag and is only a member of the rest. The one it
        // hangs off is its rarest, which is the one that says the most about
        // it: hanging everything off `documents` would draw a fan, not a map.
        QString owner;
        int ownerDegree = INT_MAX;
        for (const QString &tag : record.tags) {
            if (!tagIndex.contains(tag))
                continue;
            const int degree = tagDegree.value(tag);
            if (degree < ownerDegree || (degree == ownerDegree && tag < owner)) {
                owner = tag;
                ownerDegree = degree;
            }
        }
        if (owner.isEmpty())
            continue;
        ++m_taggedFiles;
        if (m_taggedFiles > kMaxFiles)
            continue;

        Node node;
        node.id = QStringLiteral("file:") + record.path;
        node.kind = QStringLiteral("file");
        node.label = QFileInfo(record.path).fileName();
        node.owner = tagIndex.value(owner);
        const int at = m_nodes.size();
        m_nodes.append(node);

        m_edges.push_back({node.owner, at, Branch});
        for (const QString &tag : record.tags) {
            if (tag == owner || !tagIndex.contains(tag))
                continue;
            m_edges.push_back({tagIndex.value(tag), at, Member});
        }
    }

    // Tag-tag: they share at least one file.
    QSet<QString> seen;
    for (const Record &record : std::as_const(live)) {
        const QStringList tags = record.tags;
        for (int i = 0; i < tags.size(); ++i) {
            for (int j = i + 1; j < tags.size(); ++j) {
                if (!tagIndex.contains(tags.at(i)) || !tagIndex.contains(tags.at(j)))
                    continue;
                const int a = qMin(tagIndex.value(tags.at(i)), tagIndex.value(tags.at(j)));
                const int b = qMax(tagIndex.value(tags.at(i)), tagIndex.value(tags.at(j)));
                const QString key = QStringLiteral("%1-%2").arg(a).arg(b);
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                m_edges.push_back({a, b, Shared});
            }
        }
    }

    layout();
}

// A mindmap, not two columns: tags down one side and files down the other
// makes every relationship a long diagonal, and forty of them cross in the
// middle. Each tag gets a wedge of the circle sized by how many files hang off
// it, sits at the mouth of that wedge, and its files fan out beyond it. Related
// things end up near each other, which is the only thing a picture of a graph
// is for.
void GraphItem::layout()
{
    const qreal w = qMax(width(), 1.0);
    const qreal h = qMax(height(), 1.0);
    const QPointF centre(w / 2.0, h / 2.0);

    QVector<int> tags;
    QHash<int, QVector<int>> leaves;
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes.at(i).kind == QLatin1String("tag"))
            tags.append(i);
        else if (m_nodes.at(i).owner >= 0)
            leaves[m_nodes.at(i).owner].append(i);
    }
    if (tags.isEmpty())
        return;

    int deepest = 1;
    for (int tag : std::as_const(tags))
        deepest = qMax(deepest, leaves.value(tag).size());

    // Elliptical rather than circular. A circle inscribed in a window twice as
    // wide as it is tall leaves half the window empty and crowds the map into
    // the middle; the ellipse uses what is there.
    // Room for the names, which hang outside the outermost ring and are as long
    // as the file is called.
    const qreal marginX = 132.0;
    const qreal marginY = 62.0;
    const qreal outerX = qMax(60.0, w / 2.0 - marginX);
    const qreal outerY = qMax(50.0, h / 2.0 - marginY);

    // How many rings the busiest branch needs, so the innermost one is not
    // pushed against the tags when one tag owns a dozen files.
    const int ringsNeeded = qMax(1, (deepest + 5) / 6);
    const qreal tagShare = 0.40;
    // The outermost ring always reaches the edge of the room there is. Leaving
    // it short draws the whole map into a band across the middle and wastes the
    // window, which is the one thing this view has plenty of.
    const qreal leafFirst = ringsNeeded > 1 ? 0.62 : 1.0;
    const qreal ringGap = ringsNeeded > 1 ? (1.0 - leafFirst) / (ringsNeeded - 1) : 0.0;

    // How wide each tag's slice of the circle is.
    //
    // Sized by the files hanging off it, but never narrower than its own name,
    // or two one-file tags land at neighbouring angles with their labels drawn
    // over each other. The width is estimated -- this runs before there is a
    // painter to measure with -- but a monospace face at a known size is an
    // estimate that holds.
    const qreal perChar = 6.6;
    const qreal ringRadius = qMax(60.0, (outerX + outerY) / 2.0 * tagShare);

    QVector<qreal> wedges(tags.size());
    qreal floors = 0;
    qreal weights = 0;
    for (int i = 0; i < tags.size(); ++i) {
        const int label = m_nodes.at(tags.at(i)).label.size() + 1;   // the `#`
        wedges[i] = (label * perChar + 14.0) / ringRadius;
        floors += wedges.at(i);
        weights += qMax(1, leaves.value(tags.at(i)).size());
    }

    if (floors >= 2.0 * M_PI) {
        // More names than circumference. Nothing fits properly, so share it out
        // by name length and let the view be panned and zoomed.
        for (qreal &wedge : wedges)
            wedge *= 2.0 * M_PI / floors;
    } else {
        const qreal spare = 2.0 * M_PI - floors;
        for (int i = 0; i < tags.size(); ++i)
            wedges[i] += spare * qMax(1, leaves.value(tags.at(i)).size()) / weights;
    }

    qreal cursor = -M_PI / 2.0;                 // start at the top
    for (int i = 0; i < tags.size(); ++i) {
        const int tag = tags.at(i);
        const QVector<int> &kids = leaves.value(tag);
        const qreal wedge = wedges.at(i);
        const qreal mid = cursor + wedge / 2.0;

        m_nodes[tag].pos = centre + QPointF(qCos(mid) * outerX * tagShare,
                                             qSin(mid) * outerY * tagShare);

        // Wide wedges hold more files side by side than narrow ones; a fixed
        // count per ring would fan a big branch into a solid block and leave a
        // small one with a single file on a ring of its own.
        const int perRing = qBound(1, int(wedge / 0.16), 6);
        for (int i = 0; i < kids.size(); ++i) {
            const int ring = i / perRing;
            const int slot = i % perRing;
            const int inRing = qMin(perRing, kids.size() - ring * perRing);
            // Inset from the wedge edges so two neighbouring branches do not
            // put a file each on the same line.
            const qreal t = inRing == 1 ? 0.5 : (slot + 0.5) / inRing;
            const qreal angle = cursor + wedge * (0.12 + t * 0.76);
            const qreal share = qMin(1.0, leafFirst + ring * ringGap);
            m_nodes[kids.at(i)].pos = centre + QPointF(qCos(angle) * outerX * share,
                                                        qSin(angle) * outerY * share);
        }
        cursor += wedge;
    }
}

QPointF GraphItem::toView(QPointF graph) const
{
    const QPointF center(width() / 2.0, height() / 2.0);
    return (graph - center) * m_scale + center + m_pan;
}

// Where a node's name is drawn, in view coordinates. Written once because the
// hit test and the painter have to agree about it.

// A tag is drawn as `#work`, a file as its filename.
//
// The shape and colour of the dot above the name -- a circle against a rounded
// square, at eleven pixels -- is not a distinction anybody should have to make.
// The `#` already means "tag" everywhere else, and it costs one character.
//
// It is only the drawn form. The tag a node stands for is `node.label`, and
// that is what activating one hands to the tab; putting the `#` in the name
// itself would filter for a tag nobody has.
QString GraphItem::displayLabel(const Node &node)
{
    if (node.kind == QLatin1String("tag"))
        return QLatin1Char('#') + node.label;
    return node.label;
}

QRectF GraphItem::labelRect(const Node &node, const QFontMetricsF &fm) const
{
    const QPointF p = toView(node.pos);
    const bool isTag = node.kind == QLatin1String("tag");
    const qreal r = (isTag ? 11.0 : 8.0) * m_scale;
    const qreal padX = 6.0 * m_scale;
    const qreal padY = 3.0 * m_scale;
    const QRectF bound = fm.boundingRect(displayLabel(node));
    const qreal chipW = bound.width() + padX * 2;
    const qreal chipH = bound.height() + padY * 2;

    // Along the spoke: tag names sit towards the middle, which is empty, and
    // file names sit further out, where there is nothing beyond them. Pushing
    // both the same way would stack every name on the ring outside it.
    const QPointF centre(width() / 2.0, height() / 2.0);
    QPointF away = toView(node.pos) - (centre + m_pan);
    const qreal len = std::hypot(away.x(), away.y());
    if (len < 0.001)
        away = QPointF(0, 1);
    else
        away /= len;
    const QPointF step = away * (r + 4.0 * m_scale + chipH / 2.0);
    const QPointF at = isTag ? p - step : p + step;

    return QRectF(at.x() - chipW / 2.0, at.y() - chipH / 2.0, chipW, chipH);
}

int GraphItem::hitNode(QPointF view) const
{
    int best = -1;
    qreal bestD = 1e12;
    QFont font;
    font.setFamily(QStringLiteral("monospace"));
    font.setPixelSize(qMax(9, int(11 * m_scale)));
    const QFontMetricsF fm(font);
    for (int i = 0; i < m_nodes.size(); ++i) {
        const Node &node = m_nodes.at(i);
        const QPointF p = toView(node.pos);
        const qreal r = ((node.kind == QLatin1String("tag")) ? 11.0 : 8.0) * m_scale;
        const QRectF chip = labelRect(node, fm);
        const QRectF glyph(p.x() - r - 6, p.y() - r - 6, r * 2 + 12, r * 2 + 12);
        if (!glyph.contains(view) && !chip.contains(view))
            continue;
        const QPointF d = view - p;
        const qreal dist = d.x() * d.x() + d.y() * d.y();
        if (dist < bestD) {
            bestD = dist;
            best = i;
        }
    }
    return best;
}

// Nothing to draw is three different answers, and only one of them is fixed by
// tagging something. An index with rows in it whose files cannot be reached
// wants the CLI's other vocabulary: `prune` drops a row whose file is gone, and
// it must not be the advice while the system is merely declining to describe
// one -- `verify` is the verb that says which is which.
QString GraphItem::emptyText() const
{
    const int rows = m_index ? m_index->fileCount() + m_index->missingRows()
                                   + m_index->unknownRows()
                             : 0;
    if (rows == 0 || m_index->fileCount() > 0)
        return QStringLiteral("no tagged files yet\nomafiles tag add --tag work -- FILE");
    if (m_index->unknownRows() > 0)
        return QStringLiteral("%1 tagged rows; none can be reached\nomafiles verify").arg(rows);
    return QStringLiteral("%1 tagged rows; none of their files are there\nomafiles prune")
        .arg(rows);
}

void GraphItem::paint(QPainter *painter)
{
    ensureNodes();

    // The map is drawn in the desktop's colours, like every other panel: tags
    // in the accent, files in the foreground, everything faint in the muted
    // one. The fallbacks stand for the moment between construction and the
    // binding that hands this item the theme.
    const QColor accent = m_theme ? m_theme->accent() : QColor(QStringLiteral("#7aa2f7"));
    const QColor ink = m_theme ? m_theme->foreground() : QColor(QStringLiteral("#c0caf5"));
    const QColor faint = m_theme ? m_theme->dim() : QColor(QStringLiteral("#565f89"));
    const QColor backdrop = m_theme ? m_theme->background() : QColor(QStringLiteral("#16161e"));
    const auto wash = [](QColor colour, int alpha) {
        colour.setAlpha(alpha);
        return colour;
    };

    painter->fillRect(boundingRect(), backdrop);

    if (m_nodes.isEmpty()) {
        painter->setPen(faint);
        painter->drawText(boundingRect(), Qt::AlignCenter, emptyText());
        return;
    }
    QSet<int> hot;
    int selected = -1;
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes.at(i).id == m_selectedId)
            selected = i;
        if (m_nodes.at(i).id == m_hoverId)
            hot.insert(i);
    }
    if (selected >= 0)
        hot.insert(selected);
    if (!hot.isEmpty()) {
        for (const Edge &edge : m_edges) {
            if (hot.contains(edge.from))
                hot.insert(edge.to);
            if (hot.contains(edge.to))
                hot.insert(edge.from);
        }
    }
    const bool dimming = !hot.isEmpty();

    painter->setRenderHint(QPainter::Antialiasing, true);
    const QPointF centre(width() / 2.0, height() / 2.0);
    for (const Edge &edge : m_edges) {
        const bool touched = hot.contains(edge.from) || hot.contains(edge.to);

        // The branches are the map. A file's other tags and the tags that
        // co-occur are true and worth seeing, but drawing all of them at once
        // turns this view into a ball of string, so they only appear around
        // whatever you are pointing at.
        if (edge.kind != Branch && !touched)
            continue;

        const QPointF a = toView(m_nodes.at(edge.from).pos);
        const QPointF b = toView(m_nodes.at(edge.to).pos);
        const bool live = !dimming || touched;

        QPen pen;
        if (edge.kind == Shared) {
            pen.setColor(wash(accent, live ? 130 : 50));
            pen.setStyle(Qt::DashLine);
            pen.setWidthF(1.0);
        } else if (edge.kind == Member) {
            pen.setColor(wash(ink, live ? 70 : 25));
            pen.setStyle(Qt::DotLine);
            pen.setWidthF(1.0);
        } else {
            pen.setColor(wash(ink, live ? (dimming ? 120 : 40) : 18));
            pen.setWidthF(1.4);
        }
        painter->setPen(pen);

        // Bowed away from the middle, so a branch reads as an arm off its tag
        // rather than as a chord across the map.
        QPainterPath path;
        path.moveTo(a);
        const QPointF mid((a.x() + b.x()) / 2.0, (a.y() + b.y()) / 2.0);
        QPointF out = mid - (centre + m_pan);
        const qreal len = std::hypot(out.x(), out.y());
        const QPointF bow = len < 0.001 ? QPointF() : out / len * (len * 0.12);
        path.quadTo(mid + bow, b);
        painter->drawPath(path);
    }

    for (int i = 0; i < m_nodes.size(); ++i) {
        const Node &node = m_nodes.at(i);
        const QPointF p = toView(node.pos);
        const bool isTag = node.kind == QLatin1String("tag");
        const bool live = !dimming || hot.contains(i);
        QColor fill = isTag ? accent : ink;
        if (!live)
            fill.setAlpha(40);
        else if (node.id == m_selectedId)
            fill.setAlpha(255);
        else
            fill.setAlpha(210);
        const qreal r = (isTag ? 11.0 : 8.0) * m_scale;
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        if (isTag)
            painter->drawEllipse(p, r, r);
        else
            painter->drawRoundedRect(QRectF(p.x() - r, p.y() - r, r * 2, r * 2), 3, 3);

        QFont font = painter->font();
        font.setPixelSize(qMax(9, int(11 * m_scale)));
        font.setFamily(QStringLiteral("monospace"));
        painter->setFont(font);
        const QFontMetricsF fm(font);
        // The whole name. Cutting it at 28 characters is exactly where
        // `Project Room.ods` and `Project: Remodel Room.ods` stop being
        // told apart. The view pans and zooms; a long name is a thing to move
        // the map for, not a thing to hide.
        const QRectF chip = labelRect(node, fm);

        QColor chipBg = wash(backdrop, live ? 240 : 90);
        QColor chipBorder = live ? accent : faint;
        QColor chipInk = live ? ink : faint;
        const int rounding = m_theme ? m_theme->rounding() : 0;
        painter->setPen(QPen(chipBorder, 1));
        painter->setBrush(chipBg);
        painter->drawRoundedRect(chip, rounding, rounding);
        painter->setPen(chipInk);
        painter->drawText(chip, Qt::AlignCenter, displayLabel(node));
    }
}

void GraphItem::mousePressEvent(QMouseEvent *event)
{
    m_lastMouse = event->position();
    const int hit = hitNode(event->position());
    if (hit >= 0)
        setSelectedId(m_nodes.at(hit).id);

    if (event->button() == Qt::RightButton) {
        m_dragging = false;
        if (hit >= 0) {
            const Node &node = m_nodes.at(hit);
            const QString path = node.kind == QLatin1String("file") ? node.id.mid(5) : QString();
            emit nodeMenuRequested(node.id, node.kind, path,
                                   event->position().x(), event->position().y());
        }
        return;
    }

    m_dragging = !(event->button() == Qt::LeftButton && hit >= 0);
}

void GraphItem::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_pan += event->position() - m_lastMouse;
        m_lastMouse = event->position();
        emit viewChanged();
        update();
        return;
    }
    const int hit = hitNode(event->position());
    const QString next = hit >= 0 ? m_nodes.at(hit).id : QString();
    if (next != m_hoverId) {
        m_hoverId = next;
        update();
    }
}

void GraphItem::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_dragging = false;
}

void GraphItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int hit = hitNode(event->position());
    if (hit < 0)
        return;
    const Node &node = m_nodes.at(hit);
    setSelectedId(node.id);
    emit nodeActivated(node.id, node.kind, node.kind == QLatin1String("file")
                                                ? node.id.mid(5)
                                                : node.label);
}

int GraphItem::selectedRow() const
{
    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes.at(i).id == m_selectedId)
            return i;
    }
    return -1;
}

// A map that draws part of the index says which part. `300 of 512 files` is the
// difference between a view of everything and a view of three hundred things,
// and the second number is the one the header counts.
QString GraphItem::summary() const
{
    const int drawn = m_nodes.size() - m_tagCount;
    const QString files = drawn == m_taggedFiles
        ? QStringLiteral("%1 files").arg(m_taggedFiles)
        : QStringLiteral("%1 of %2 files").arg(drawn).arg(m_taggedFiles);
    return QStringLiteral("%1 tags · %2").arg(m_tagCount).arg(files);
}

void GraphItem::selectIndex(int index)
{
    if (index < 0 || index >= m_nodes.size())
        return;
    setSelectedId(m_nodes.at(index).id);
}

// Wraps, so holding the key walks the whole graph. There is no geometric next
// node to step to -- the layout is a cloud, not a column -- so this walks the
// node order, which is stable for as long as the index is.
void GraphItem::stepSelection(int delta)
{
    ensureNodes();
    if (m_nodes.isEmpty() || delta == 0)
        return;
    const int count = m_nodes.size();
    const int at = selectedRow();
    if (at < 0) {
        selectIndex(delta > 0 ? 0 : count - 1);
        return;
    }
    selectIndex(((at + delta) % count + count) % count);
}

void GraphItem::selectEnd(bool last)
{
    ensureNodes();
    if (m_nodes.isEmpty())
        return;
    selectIndex(last ? m_nodes.size() - 1 : 0);
}

void GraphItem::activateSelection()
{
    ensureNodes();
    const int at = selectedRow();
    if (at < 0)
        return;
    const Node &node = m_nodes.at(at);
    const bool isFile = node.kind == QLatin1String("file");
    emit nodeActivated(node.id, node.kind, isFile ? node.id.mid(5) : node.label);
}

void GraphItem::menuForSelection()
{
    ensureNodes();
    const int at = selectedRow();
    if (at < 0)
        return;
    const Node &node = m_nodes.at(at);
    if (node.kind != QLatin1String("file"))
        return;
    const QPointF p = toView(node.pos);
    emit nodeMenuRequested(node.id, node.kind, node.id.mid(5), p.x(), p.y());
}

void GraphItem::zoomBy(qreal factor)
{
    const qreal next = qBound(0.4, m_scale * factor, 3.0);
    if (qFuzzyCompare(next, m_scale))
        return;
    m_scale = next;
    emit viewChanged();
    update();
}

void GraphItem::panBy(qreal dx, qreal dy)
{
    m_pan += QPointF(dx, dy);
    emit viewChanged();
    update();
}

QPointF GraphItem::pan() const
{
    return m_pan;
}

void GraphItem::wheelEvent(QWheelEvent *event)
{
    const qreal steps = event->angleDelta().y() / 120.0;
    const qreal next = qBound(0.4, m_scale * (1.0 + steps * 0.12), 3.0);
    if (qFuzzyCompare(next, m_scale))
        return;
    m_scale = next;
    emit viewChanged();
    update();
}

void GraphItem::hoverMoveEvent(QHoverEvent *event)
{
    const int hit = hitNode(event->position());
    const QString next = hit >= 0 ? m_nodes.at(hit).id : QString();
    if (next == m_hoverId)
        return;
    m_hoverId = next;
    update();
}

} // namespace omafiles
