#include "designer/design_scene.h"

#include "designer/canvas_item.h"
#include "runtime/widgets/display_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QGraphicsSceneDragDropEvent>
#include <QtWidgets/QGraphicsSceneMouseEvent>

#include <algorithm>
#include <functional>
#include <numeric>

namespace fhmi {

DesignScene::DesignScene(QObject *parent) : QGraphicsScene(parent)
{
    setCanvasSize(m_canvasSize);
}

void DesignScene::setCanvasSize(const QSize &size)
{
    m_canvasSize = size;
    // A margin around the page leaves room to drag items past its edge.
    setSceneRect(QRectF(-40, -40, size.width() + 80, size.height() + 80));
    update();
}

void DesignScene::setGridSize(int size)
{
    m_gridSize = std::max(1, size);
    for (CanvasItem *item : topLevelItems()) {
        item->setGridSize(m_gridSize);
    }
    update();
}

void DesignScene::setGridVisible(bool visible)
{
    m_gridVisible = visible;
    update();
}

void DesignScene::setAssetBaseDir(const QString &dir)
{
    m_assetBaseDir = dir;
    for (CanvasItem *item : topLevelItems()) {
        item->setAssetBaseDir(dir);
    }
}

void DesignScene::drawBackground(QPainter *painter, const QRectF &rect)
{
    const bool dark = isDarkTheme();
    painter->fillRect(rect, dark ? QColor(0x20, 0x20, 0x20) : QColor(0xe9, 0xec, 0xf0));
    const QRectF page(QPointF(0, 0), QSizeF(m_canvasSize));
    painter->fillRect(page, dark ? QColor(0x2b, 0x2b, 0x2b) : QColor(0xfb, 0xfb, 0xfb));
    if (m_gridVisible && m_gridSize >= 4) {
        const QRectF area = rect.intersected(page);
        const int step = m_gridSize < 8 ? m_gridSize * 2 : m_gridSize;
        painter->setPen(QPen(dark ? QColor(255, 255, 255, 28) : QColor(0, 0, 0, 30), 1));
        QVector<QPointF> dots;
        for (int x = int(std::floor(area.left() / step)) * step; x <= area.right(); x += step) {
            for (int y = int(std::floor(area.top() / step)) * step; y <= area.bottom(); y += step) {
                dots.append(QPointF(x, y));
            }
        }
        painter->drawPoints(dots.constData(), int(dots.size()));
    }
    painter->setPen(QPen(dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 40), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(page);
}

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------

void DesignScene::adopt(CanvasItem *item)
{
    item->setGridSize(m_gridSize);
    connect(item, &CanvasItem::changed, this, [this](CanvasItem *changed) {
        reparentAfterMove(changed);
        emit itemPropertiesChanged(changed);
        emit modified();
    });
    for (CanvasItem *child : item->containerChildren()) {
        adopt(child);
    }
}

bool DesignScene::loadWidgets(const QJsonArray &widgets, QString *error)
{
    QList<CanvasItem *> staged;
    for (const QJsonValue &value : widgets) {
        CanvasItem *item = CanvasItem::fromJson(value.toObject(), m_assetBaseDir);
        if (!item) {
            qDeleteAll(staged);
            if (error) {
                *error = tr("无法创建控件: %1").arg(value.toObject().value(QStringLiteral("type")).toString());
            }
            return false;
        }
        staged.append(item);
    }
    clearAll();
    for (CanvasItem *item : std::as_const(staged)) {
        addItem(item);
        adopt(item);
    }
    return true;
}

QJsonArray DesignScene::widgetsJson() const
{
    QJsonArray out;
    for (const CanvasItem *item : topLevelItems()) {
        out.append(item->toJson());
    }
    return out;
}

QList<CanvasItem *> DesignScene::topLevelItems() const
{
    QList<CanvasItem *> out;
    // items() is in descending stacking order; projects store bottom-most first.
    const QList<QGraphicsItem *> all = items(Qt::AscendingOrder);
    for (QGraphicsItem *item : all) {
        if (item->type() == CanvasItem::Type && !item->parentItem()) {
            out.append(static_cast<CanvasItem *>(item));
        }
    }
    return out;
}

QList<CanvasItem *> DesignScene::allItems() const
{
    QList<CanvasItem *> out;
    std::function<void(CanvasItem *)> walk = [&](CanvasItem *item) {
        out.append(item);
        for (CanvasItem *child : item->containerChildren()) {
            walk(child);
        }
    };
    for (CanvasItem *item : topLevelItems()) {
        walk(item);
    }
    return out;
}

QList<CanvasItem *> DesignScene::selectedCanvasItems() const
{
    QList<CanvasItem *> out;
    for (QGraphicsItem *item : selectedItems()) {
        if (item->type() == CanvasItem::Type) {
            out.append(static_cast<CanvasItem *>(item));
        }
    }
    return out;
}

CanvasItem *DesignScene::findItem(const QString &id) const
{
    for (CanvasItem *item : allItems()) {
        if (item->widgetId() == id) {
            return item;
        }
    }
    return nullptr;
}

QString DesignScene::uniqueId(const QString &typeName) const
{
    QSet<QString> used;
    for (const CanvasItem *item : allItems()) {
        used.insert(item->widgetId());
    }
    QString base = typeName;
    base[0] = base.at(0).toLower();
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("%1%2").arg(base).arg(n);
        if (!used.contains(candidate)) {
            return candidate;
        }
    }
}

bool DesignScene::renameItem(CanvasItem *item, const QString &id, QString *error)
{
    const QString trimmed = id.trimmed();
    if (trimmed.isEmpty()) {
        *error = tr("控件 ID 不能为空");
        return false;
    }
    if (CanvasItem *other = findItem(trimmed); other && other != item) {
        *error = tr("控件 ID “%1” 已被使用").arg(trimmed);
        return false;
    }
    item->setWidgetId(trimmed);
    emit modified();
    return true;
}

CanvasItem *DesignScene::createItem(const QString &typeName, const QPointF &scenePos)
{
    if (!WidgetRegistry::instance().find(typeName)) {
        return nullptr;
    }
    const WidgetMeta *meta = WidgetRegistry::instance().find(typeName);
    auto *item = new CanvasItem(typeName, meta->defaultProperties(), uniqueId(typeName), m_assetBaseDir);
    const qreal g = m_gridSize;
    item->setPos(std::round(scenePos.x() / g) * g, std::round(scenePos.y() / g) * g);
    addItem(item);
    adopt(item);
    if (CanvasItem *container = containerAt(scenePos, item)) {
        container->addChild(item);
    }
    clearSelection();
    item->setSelected(true);
    emit modified();
    return item;
}

void DesignScene::deleteSelected()
{
    QList<CanvasItem *> doomed = selectedCanvasItems();
    // Deleting a container deletes its children; skip those already covered.
    doomed.erase(std::remove_if(doomed.begin(), doomed.end(),
                                [&doomed](CanvasItem *item) {
                                    return std::any_of(doomed.begin(), doomed.end(), [item](CanvasItem *other) {
                                        return other != item && other->isAncestorOf(item);
                                    });
                                }),
                 doomed.end());
    if (doomed.isEmpty()) {
        return;
    }
    for (CanvasItem *item : std::as_const(doomed)) {
        if (item == m_dropTarget) {
            m_dropTarget = nullptr;
        }
        CanvasItem *parent = item->containerParent();
        delete item;
        if (parent) {
            parent->relayoutChildren();
        }
    }
    emit modified();
}

void DesignScene::clearAll()
{
    m_dropTarget = nullptr;
    for (CanvasItem *item : topLevelItems()) {
        delete item;
    }
}

void DesignScene::align(Alignment alignment)
{
    const QList<CanvasItem *> items = selectedCanvasItems();
    if (items.size() < 2) {
        return;
    }
    // Scene coordinates, so items in different containers still line up.
    QList<QRectF> rects;
    for (const CanvasItem *item : items) {
        rects.append(QRectF(item->scenePos(), item->size()));
    }
    qreal left = rects.first().left(), right = rects.first().right();
    qreal top = rects.first().top(), bottom = rects.first().bottom();
    for (const QRectF &r : std::as_const(rects)) {
        left = std::min(left, r.left());
        right = std::max(right, r.right());
        top = std::min(top, r.top());
        bottom = std::max(bottom, r.bottom());
    }
    const auto moveTo = [](CanvasItem *item, const QPointF &scenePos) {
        item->setPos(item->parentItem() ? item->parentItem()->mapFromScene(scenePos) : scenePos);
    };
    QList<int> order(items.size());
    std::iota(order.begin(), order.end(), 0);
    switch (alignment) {
    case Alignment::Left:
        for (int i = 0; i < items.size(); ++i) {
            moveTo(items[i], QPointF(left, rects[i].top()));
        }
        break;
    case Alignment::Right:
        for (int i = 0; i < items.size(); ++i) {
            moveTo(items[i], QPointF(right - rects[i].width(), rects[i].top()));
        }
        break;
    case Alignment::Top:
        for (int i = 0; i < items.size(); ++i) {
            moveTo(items[i], QPointF(rects[i].left(), top));
        }
        break;
    case Alignment::Bottom:
        for (int i = 0; i < items.size(); ++i) {
            moveTo(items[i], QPointF(rects[i].left(), bottom - rects[i].height()));
        }
        break;
    case Alignment::HorizontalCenter:
        for (int i = 0; i < items.size(); ++i) {
            moveTo(items[i], QPointF((left + right - rects[i].width()) / 2, rects[i].top()));
        }
        break;
    case Alignment::VerticalCenter:
        for (int i = 0; i < items.size(); ++i) {
            moveTo(items[i], QPointF(rects[i].left(), (top + bottom - rects[i].height()) / 2));
        }
        break;
    case Alignment::DistributeHorizontally:
    case Alignment::DistributeVertically: {
        if (items.size() < 3) {
            return;
        }
        const bool horizontal = alignment == Alignment::DistributeHorizontally;
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return horizontal ? rects[a].left() < rects[b].left() : rects[a].top() < rects[b].top();
        });
        qreal total = 0;
        for (const QRectF &r : std::as_const(rects)) {
            total += horizontal ? r.width() : r.height();
        }
        const qreal span = horizontal ? right - left : bottom - top;
        const qreal gap = (span - total) / (items.size() - 1);
        qreal cursor = horizontal ? left : top;
        for (int index : std::as_const(order)) {
            const QRectF &r = rects[index];
            moveTo(items[index], horizontal ? QPointF(cursor, r.top()) : QPointF(r.left(), cursor));
            cursor += (horizontal ? r.width() : r.height()) + gap;
        }
        break;
    }
    }
    for (CanvasItem *item : items) {
        if (item->containerParent()) {
            item->containerParent()->relayoutChildren();
        }
    }
    emit modified();
}

// ---------------------------------------------------------------------------
// Containers and drag & drop
// ---------------------------------------------------------------------------

CanvasItem *DesignScene::containerAt(const QPointF &scenePos, const CanvasItem *exclude) const
{
    // items() at a point is topmost first, so the first container hit is the innermost.
    for (QGraphicsItem *hit : items(scenePos)) {
        if (hit->type() != CanvasItem::Type || !hit->isVisible()) {
            continue;
        }
        auto *item = static_cast<CanvasItem *>(hit);
        if (!item->isContainer() || item == exclude || (exclude && exclude->isAncestorOf(item))) {
            continue;
        }
        if (item->rect().contains(item->mapFromScene(scenePos))) {
            return item;
        }
    }
    return nullptr;
}

void DesignScene::highlightDropTarget(CanvasItem *target)
{
    if (m_dropTarget == target) {
        return;
    }
    if (m_dropTarget) {
        m_dropTarget->setDropHighlight(false);
    }
    m_dropTarget = target;
    if (m_dropTarget) {
        m_dropTarget->setDropHighlight(true);
    }
}

void DesignScene::reparentAfterMove(CanvasItem *item)
{
    highlightDropTarget(nullptr);
    if (!item || item->scene() != this) {
        return;
    }
    const QPointF center = item->mapToScene(item->rect().center());
    CanvasItem *target = containerAt(center, item);
    if (target == item->containerParent()) {
        return;
    }
    if (target) {
        target->addChild(item);
    } else if (CanvasItem *parent = item->containerParent()) {
        parent->removeChild(item);
    }
    item->snapToGrid();
}

void DesignScene::dragEnterEvent(QGraphicsSceneDragDropEvent *event)
{
    if (event->mimeData()->hasFormat(QLatin1String(WidgetMimeType))) {
        event->acceptProposedAction();
    } else {
        QGraphicsScene::dragEnterEvent(event);
    }
}

void DesignScene::dragMoveEvent(QGraphicsSceneDragDropEvent *event)
{
    if (event->mimeData()->hasFormat(QLatin1String(WidgetMimeType))) {
        highlightDropTarget(containerAt(event->scenePos()));
        event->acceptProposedAction();
    } else {
        QGraphicsScene::dragMoveEvent(event);
    }
}

void DesignScene::dragLeaveEvent(QGraphicsSceneDragDropEvent *event)
{
    highlightDropTarget(nullptr);
    QGraphicsScene::dragLeaveEvent(event);
}

void DesignScene::dropEvent(QGraphicsSceneDragDropEvent *event)
{
    highlightDropTarget(nullptr);
    if (!event->mimeData()->hasFormat(QLatin1String(WidgetMimeType))) {
        QGraphicsScene::dropEvent(event);
        return;
    }
    const QString type = QString::fromUtf8(event->mimeData()->data(QLatin1String(WidgetMimeType)));
    createItem(type, event->scenePos());
    event->acceptProposedAction();
}

void DesignScene::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        deleteSelected();
        event->accept();
        return;
    }
    // Arrow keys nudge the selection by one pixel, or a grid step with Shift.
    const int step = (event->modifiers() & Qt::ShiftModifier) ? m_gridSize : 1;
    QPointF delta;
    switch (event->key()) {
    case Qt::Key_Left:
        delta = QPointF(-step, 0);
        break;
    case Qt::Key_Right:
        delta = QPointF(step, 0);
        break;
    case Qt::Key_Up:
        delta = QPointF(0, -step);
        break;
    case Qt::Key_Down:
        delta = QPointF(0, step);
        break;
    default:
        QGraphicsScene::keyPressEvent(event);
        return;
    }
    const QList<CanvasItem *> items = selectedCanvasItems();
    for (CanvasItem *item : items) {
        if (item->flags() & QGraphicsItem::ItemIsMovable) {
            item->setPos(item->pos() + delta);
            emit itemPropertiesChanged(item);
        }
    }
    if (!items.isEmpty()) {
        emit modified();
    }
    event->accept();
}

void DesignScene::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    QGraphicsScene::mouseReleaseEvent(event);
    highlightDropTarget(nullptr);
}

} // namespace fhmi
