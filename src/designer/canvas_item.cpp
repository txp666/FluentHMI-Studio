#include "designer/canvas_item.h"

#include "runtime/hmi_widget.h"
#include "runtime/widgets/container_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QUuid>
#include <QtGui/QKeyEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QGraphicsProxyWidget>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsSceneContextMenuEvent>
#include <QtWidgets/QGraphicsSceneDragDropEvent>
#include <QtWidgets/QGraphicsSceneMouseEvent>
#include <QtWidgets/QGraphicsSceneWheelEvent>
#include <QtWidgets/QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>

namespace fhmi {

namespace {

constexpr qreal kHandle = 8;
const QColor kAccent(0, 120, 212);

// Shows the widget but never hands it input: the wheel scrolls the canvas, and a drop
// lands on the canvas, instead of zooming a chart or reaching a combo box.
class InertProxy : public QGraphicsProxyWidget
{
  public:
    using QGraphicsProxyWidget::QGraphicsProxyWidget;

  protected:
    void wheelEvent(QGraphicsSceneWheelEvent *event) override { event->ignore(); }
    void contextMenuEvent(QGraphicsSceneContextMenuEvent *event) override { event->ignore(); }
    void dragEnterEvent(QGraphicsSceneDragDropEvent *event) override { event->ignore(); }
    void dragMoveEvent(QGraphicsSceneDragDropEvent *event) override { event->ignore(); }
    void dropEvent(QGraphicsSceneDragDropEvent *event) override { event->ignore(); }
    void keyPressEvent(QKeyEvent *event) override { event->ignore(); }
};

} // namespace

CanvasItem::CanvasItem(const QString &typeName, const QJsonObject &properties, const QString &id,
                       const QString &assetBaseDir)
    : m_meta(WidgetRegistry::instance().find(typeName))
{
    Q_ASSERT(m_meta);
    m_id = id.isEmpty() ? QStringLiteral("%1_%2").arg(typeName, QUuid::createUuid().toString(QUuid::Id128).left(6))
                        : id;
    // Only what the project stores; readers fall back to the metadata defaults, so a
    // loaded project saves back without growing every default property.
    m_properties = normalizedRangeProperties(typeName, upgradedProperties(typeName, properties));
    m_size = QSizeF(m_meta->defaultWidth, m_meta->defaultHeight);

    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);

    m_widget = createHmiWidget(typeName, m_properties, nullptr, assetBaseDir);
    m_widget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // Embedded widgets are windows, and Qt paints a window background behind them: a label
    // would hide the grid and, in the dark theme, show up as a box. At run time they sit on
    // the surface without one, so the canvas shows them the same way.
    m_widget->setAttribute(Qt::WA_TranslucentBackground, true);
    m_proxy = new InertProxy(this);
    m_proxy->setWidget(m_widget);
    m_proxy->setAcceptedMouseButtons(Qt::NoButton);
    m_proxy->setAcceptHoverEvents(false);
    // Behind this item's own painting, so the selection outline shows over the widget.
    m_proxy->setFlag(ItemStacksBehindParent, true);
    m_proxy->setFocusPolicy(Qt::NoFocus);
    syncWidget();
}

CanvasItem::~CanvasItem()
{
    if (m_containerParent) {
        m_containerParent->m_children.removeAll(this);
    }
    for (CanvasItem *child : std::as_const(m_children)) {
        child->m_containerParent = nullptr;
    }
}

CanvasItem *CanvasItem::fromJson(const QJsonObject &data, const QString &assetBaseDir)
{
    const QString type = data.value(QStringLiteral("type")).toString();
    if (!WidgetRegistry::instance().find(type)) {
        return nullptr;
    }
    auto *item = new CanvasItem(type, data.value(QStringLiteral("properties")).toObject(),
                                data.value(QStringLiteral("id")).toString(), assetBaseDir);
    item->m_binding = data.value(QStringLiteral("binding")).toObject();
    item->m_tabIndex = data.value(QStringLiteral("tabIndex")).toInt(0);
    const QJsonObject geo = data.value(QStringLiteral("geometry")).toObject();
    item->setPos(geo.value(QStringLiteral("x")).toDouble(0), geo.value(QStringLiteral("y")).toDouble(0));
    const qreal height = geo.value(QStringLiteral("h")).toDouble(item->m_meta->defaultHeight);
    // A link card saved shorter than its rows would show them on top of each other: it grows.
    item->setSize(QSizeF(geo.value(QStringLiteral("w")).toDouble(item->m_meta->defaultWidth),
                         item->isLinkCard() ? std::max(height, item->rowsHeight()) : height),
                  false);
    for (const QJsonValue &value : data.value(QStringLiteral("children")).toArray()) {
        const QJsonObject childData = value.toObject();
        if (CanvasItem *child = fromJson(childData, assetBaseDir)) {
            const QPointF local = child->pos();
            item->addChild(child, child->m_tabIndex, &local);
        }
    }
    item->relayoutChildren();
    return item;
}

QJsonObject CanvasItem::toJson() const
{
    QJsonObject data{
        {QStringLiteral("id"), m_id},
        {QStringLiteral("type"), typeName()},
        {QStringLiteral("geometry"),
         QJsonObject{{QStringLiteral("x"), qRound(pos().x())},
                     {QStringLiteral("y"), qRound(pos().y())},
                     {QStringLiteral("w"), qRound(m_size.width())},
                     {QStringLiteral("h"), qRound(m_size.height())}}},
        {QStringLiteral("properties"), m_properties},
    };
    if (!m_binding.isEmpty()) {
        data.insert(QStringLiteral("binding"), m_binding);
    }
    if (m_tabIndex) {
        data.insert(QStringLiteral("tabIndex"), m_tabIndex);
    }
    if (!m_children.isEmpty()) {
        QJsonArray children;
        for (const CanvasItem *child : m_children) {
            children.append(child->toJson());
        }
        data.insert(QStringLiteral("children"), children);
    }
    return data;
}

QJsonValue CanvasItem::propertyValue(const QString &name) const
{
    const QJsonValue value = m_properties.value(name);
    if (!value.isUndefined()) {
        return value;
    }
    const PropDef *def = m_meta->prop(name);
    return def ? def->defaultValue : QJsonValue();
}

void CanvasItem::setPropertyValue(const QString &name, const QJsonValue &value)
{
    QJsonObject props = m_properties;
    props.insert(name, value);
    if (typeName() == QLatin1String("TabWidget")) {
        const int count = int(tabNames(props.value(QStringLiteral("tabNames")).toString()).size());
        props.insert(QStringLiteral("currentTab"),
                     clampTabIndex(props.value(QStringLiteral("currentTab")).toInt(0), count));
        if (name == QLatin1String("tabNames")) {
            // Children on removed pages move to the last page.
            for (CanvasItem *child : std::as_const(m_children)) {
                child->m_tabIndex = clampTabIndex(child->m_tabIndex, count);
            }
        }
    }
    m_properties = normalizedRangeProperties(typeName(), props, name);
    m_widget->setProperties(m_properties);
    if (isContainer()) {
        relayoutChildren();
    }
    if (isLinkCard() && m_size.height() < minimumHeight()) {
        setSize(QSizeF(m_size.width(), minimumHeight())); // more rows (displayMode) need more height
    }
    update();
}

bool CanvasItem::isLinkCard() const
{
    return typeName() == QLatin1String("SerialConfigCard") || typeName() == QLatin1String("TcpConfigCard") ||
           typeName() == QLatin1String("MqttConfigCard");
}

qreal CanvasItem::rowsHeight() const
{
    return m_widget->minimumSizeHint().height();
}

qreal CanvasItem::minimumHeight() const
{
    const qreal height = m_meta->minHeight;
    return isLinkCard() ? std::max(height, rowsHeight()) : height;
}

void CanvasItem::setSize(const QSizeF &size, bool enforceMinimum)
{
    const QSizeF bounded = enforceMinimum ? QSizeF(std::max<qreal>(size.width(), m_meta->minWidth),
                                                   std::max<qreal>(size.height(), minimumHeight()))
                                          : QSizeF(std::max<qreal>(size.width(), 1), std::max<qreal>(size.height(), 1));
    if (bounded == m_size) {
        return;
    }
    prepareGeometryChange();
    m_size = bounded;
    syncWidget();
    if (isContainer()) {
        relayoutChildren();
    }
}

void CanvasItem::syncWidget()
{
    m_widget->setFixedSize(m_size.toSize());
    m_proxy->setPos(0, 0);
    m_proxy->resize(m_size);
}

QRectF CanvasItem::boundingRect() const
{
    const qreal m = kHandle / 2 + 1;
    return rect().adjusted(-m, -m, m, m);
}

QList<QPair<int, QRectF>> CanvasItem::handleRects() const
{
    const QRectF r = rect();
    const qreal h = kHandle / 2;
    const auto at = [h](qreal x, qreal y) { return QRectF(x - h, y - h, kHandle, kHandle); };
    return {{Left | Top, at(r.left(), r.top())},          {Top, at(r.center().x(), r.top())},
            {Right | Top, at(r.right(), r.top())},        {Right, at(r.right(), r.center().y())},
            {Right | Bottom, at(r.right(), r.bottom())},  {Bottom, at(r.center().x(), r.bottom())},
            {Left | Bottom, at(r.left(), r.bottom())},    {Left, at(r.left(), r.center().y())}};
}

int CanvasItem::handleAt(const QPointF &pos) const
{
    if (!isSelected() || !m_meta->resizable) {
        return None;
    }
    for (const auto &handle : handleRects()) {
        if (handle.second.adjusted(-2, -2, 2, 2).contains(pos)) {
            return handle.first;
        }
    }
    return None;
}

void CanvasItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    painter->setRenderHint(QPainter::Antialiasing, false);
    if (m_dropHighlight) {
        painter->setPen(QPen(kAccent, 2, Qt::DashLine));
        painter->setBrush(QColor(0, 120, 212, 25));
        painter->drawRect(rect().adjusted(1, 1, -1, -1));
    }
    if (!isSelected()) {
        return;
    }
    painter->setPen(QPen(kAccent, 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(rect());
    if (!m_meta->resizable) {
        return;
    }
    painter->setPen(QPen(Qt::white, 1));
    painter->setBrush(kAccent);
    for (const auto &handle : handleRects()) {
        painter->drawRect(handle.second);
    }
}

void CanvasItem::setGridSize(int size)
{
    m_gridSize = std::max(1, size);
    for (CanvasItem *child : std::as_const(m_children)) {
        child->setGridSize(size);
    }
}

void CanvasItem::setAssetBaseDir(const QString &dir)
{
    m_widget->setAssetBaseDir(dir);
    for (CanvasItem *child : std::as_const(m_children)) {
        child->setAssetBaseDir(dir);
    }
}

void CanvasItem::snapToGrid()
{
    if (!(flags() & ItemIsMovable)) {
        return;
    }
    const qreal g = m_gridSize;
    setPos(std::round(pos().x() / g) * g, std::round(pos().y() / g) * g);
}

QVariant CanvasItem::itemChange(GraphicsItemChange change, const QVariant &value)
{
    if (change == ItemPositionChange && m_resizeHandle == None && (flags() & ItemIsMovable) &&
        QGraphicsItem::scene() && QGraphicsItem::scene()->mouseGrabberItem() == this) {
        const QPointF p = value.toPointF();
        const qreal g = m_gridSize;
        return QPointF(std::round(p.x() / g) * g, std::round(p.y() / g) * g);
    }
    if (change == ItemSelectedHasChanged) {
        update();
    }
    return QGraphicsObject::itemChange(change, value);
}

void CanvasItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    m_moved = false;
    m_resizeHandle = event->button() == Qt::LeftButton ? handleAt(event->pos()) : None;
    if (m_resizeHandle != None) {
        m_pressScenePos = event->scenePos();
        m_pressPos = pos();
        m_pressSize = m_size;
        event->accept();
        return;
    }
    QGraphicsObject::mousePressEvent(event);
}

void CanvasItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
    m_moved = true;
    if (m_resizeHandle == None) {
        QGraphicsObject::mouseMoveEvent(event);
        return;
    }
    const QPointF delta = event->scenePos() - m_pressScenePos;
    const qreal g = m_gridSize;
    const auto snap = [g](qreal v) { return std::round(v / g) * g; };
    QPointF p = m_pressPos;
    QSizeF s = m_pressSize;
    const qreal minW = m_meta->minWidth;
    const qreal minH = minimumHeight();
    if (m_resizeHandle & Right) {
        s.setWidth(std::max(minW, snap(m_pressPos.x() + m_pressSize.width() + delta.x()) - m_pressPos.x()));
    }
    if (m_resizeHandle & Bottom) {
        s.setHeight(std::max(minH, snap(m_pressPos.y() + m_pressSize.height() + delta.y()) - m_pressPos.y()));
    }
    if (m_resizeHandle & Left) {
        const qreal right = m_pressPos.x() + m_pressSize.width();
        const qreal left = std::min(snap(m_pressPos.x() + delta.x()), right - minW);
        p.setX(left);
        s.setWidth(right - left);
    }
    if (m_resizeHandle & Top) {
        const qreal bottom = m_pressPos.y() + m_pressSize.height();
        const qreal top = std::min(snap(m_pressPos.y() + delta.y()), bottom - minH);
        p.setY(top);
        s.setHeight(bottom - top);
    }
    setPos(p);
    setSize(s);
    event->accept();
}

void CanvasItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    const bool wasResizing = m_resizeHandle != None;
    m_resizeHandle = None;
    if (wasResizing) {
        event->accept();
    } else {
        QGraphicsObject::mouseReleaseEvent(event);
    }
    if (m_moved) {
        if (m_containerParent) {
            m_containerParent->relayoutChildren();
        }
        emit changed(this);
    }
    m_moved = false;
}

void CanvasItem::hoverMoveEvent(QGraphicsSceneHoverEvent *event)
{
    const int handle = handleAt(event->pos());
    if (handle == (Left | Top) || handle == (Right | Bottom)) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (handle == (Right | Top) || handle == (Left | Bottom)) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (handle == Left || handle == Right) {
        setCursor(Qt::SizeHorCursor);
    } else if (handle == Top || handle == Bottom) {
        setCursor(Qt::SizeVerCursor);
    } else {
        setCursor((flags() & ItemIsMovable) ? Qt::SizeAllCursor : Qt::ArrowCursor);
    }
    QGraphicsObject::hoverMoveEvent(event);
}

void CanvasItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *event)
{
    unsetCursor();
    QGraphicsObject::hoverLeaveEvent(event);
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

bool CanvasItem::isAncestorOf(const CanvasItem *item) const
{
    for (const CanvasItem *p = item ? item->m_containerParent : nullptr; p; p = p->m_containerParent) {
        if (p == this) {
            return true;
        }
    }
    return false;
}

void CanvasItem::addChild(CanvasItem *child, int tabIndex, const QPointF *localPos)
{
    if (!child || child == this || child->isAncestorOf(this) || child->m_containerParent == this) {
        return;
    }
    const QPointF position = localPos ? *localPos : mapFromScene(child->scenePos());
    if (child->m_containerParent) {
        child->m_containerParent->m_children.removeAll(child);
        child->m_containerParent->relayoutChildren();
    }
    child->m_containerParent = this;
    child->setParentItem(this);
    child->setPos(position);
    child->setGridSize(m_gridSize);
    m_children.append(child);
    if (typeName() == QLatin1String("TabWidget")) {
        const int pages = int(tabNames(propertyValue(QStringLiteral("tabNames")).toString()).size());
        child->m_tabIndex = clampTabIndex(tabIndex >= 0 ? tabIndex : m_widget->currentPage(), pages);
    } else {
        child->m_tabIndex = 0;
    }
    relayoutChildren();
}

void CanvasItem::removeChild(CanvasItem *child)
{
    if (!child || child->m_containerParent != this) {
        return;
    }
    const QPointF scenePos = child->scenePos();
    m_children.removeAll(child);
    child->m_containerParent = nullptr;
    child->setParentItem(nullptr);
    child->setPos(scenePos);
    child->m_tabIndex = 0;
    child->setFlag(ItemIsMovable, true);
    child->setVisible(true);
    relayoutChildren();
}

QString CanvasItem::layoutMode() const
{
    const QString type = typeName();
    if (type == QLatin1String("HBoxLayout")) {
        return QStringLiteral("水平");
    }
    if (type == QLatin1String("VBoxLayout") || type == QLatin1String("GroupBox")) {
        return QStringLiteral("垂直");
    }
    if (type == QLatin1String("FlexLayout")) {
        return propertyValue(QStringLiteral("direction")).toString() == QLatin1String("column")
                   ? QStringLiteral("弹性列")
                   : QStringLiteral("弹性(Flex)");
    }
    if (type == QLatin1String("CardContainer")) {
        return propertyValue(QStringLiteral("layout")).toString(QStringLiteral("自由"));
    }
    if (type == QLatin1String("TabWidget")) {
        const int page = propertyValue(QStringLiteral("currentTab")).toInt(0);
        return m_properties.value(QStringLiteral("_tabLayout_%1").arg(page)).toString(QStringLiteral("自由"));
    }
    return QStringLiteral("自由");
}

bool CanvasItem::isFreeLayout() const
{
    return layoutMode() == QStringLiteral("自由");
}

void CanvasItem::fitWidth(qreal available)
{
    setSize(QSizeF(std::max<qreal>(m_meta->minWidth, available), m_size.height()));
}

void CanvasItem::relayoutChildren()
{
    if (m_children.isEmpty()) {
        return;
    }
    const bool tabs = typeName() == QLatin1String("TabWidget");
    const int page = tabs ? clampTabIndex(propertyValue(QStringLiteral("currentTab")).toInt(0),
                                          int(tabNames(propertyValue(QStringLiteral("tabNames")).toString()).size()))
                          : 0;
    QList<CanvasItem *> visible;
    for (CanvasItem *child : std::as_const(m_children)) {
        const bool shown = !tabs || child->m_tabIndex == page;
        child->setVisible(shown);
        if (shown) {
            visible.append(child);
        }
    }
    const QString mode = layoutMode();
    const bool free = mode == QStringLiteral("自由");
    for (CanvasItem *child : std::as_const(visible)) {
        child->setFlag(ItemIsMovable, free);
    }
    if (free) {
        return;
    }

    const qreal pad = ContainerLayout::Padding;
    const qreal spacing = propertyValue(QStringLiteral("spacing")).toDouble(8);
    qreal top = m_widget->contentTop();
    if (typeName() == QLatin1String("CardContainer")) {
        top = std::max(pad, top);
    }
    const qreal width = m_size.width();
    const qreal height = m_size.height();

    if (mode == QStringLiteral("水平")) {
        qreal x = pad;
        for (CanvasItem *child : std::as_const(visible)) {
            child->setPos(x, top);
            x += child->m_size.width() + spacing;
        }
    } else if (mode == QStringLiteral("垂直")) {
        qreal y = top;
        for (CanvasItem *child : std::as_const(visible)) {
            child->setPos(pad, y);
            child->fitWidth(width - 2 * pad);
            y += child->m_size.height() + spacing;
        }
    } else if (mode == QStringLiteral("弹性(Flex)")) {
        qreal x = pad, y = top, rowHeight = 0;
        for (CanvasItem *child : std::as_const(visible)) {
            if (x + child->m_size.width() > width - pad && x > pad) {
                x = pad;
                y += rowHeight + spacing;
                rowHeight = 0;
            }
            child->setPos(x, y);
            x += child->m_size.width() + spacing;
            rowHeight = std::max(rowHeight, child->m_size.height());
        }
    } else if (mode == QStringLiteral("弹性列")) {
        qreal x = pad, y = top, columnWidth = 0;
        for (CanvasItem *child : std::as_const(visible)) {
            if (y + child->m_size.height() > height - pad && y > top) {
                y = top;
                x += columnWidth + spacing;
                columnWidth = 0;
            }
            child->setPos(x, y);
            y += child->m_size.height() + spacing;
            columnWidth = std::max(columnWidth, child->m_size.width());
        }
    }
}

void CanvasItem::setDropHighlight(bool on)
{
    if (m_dropHighlight != on) {
        m_dropHighlight = on;
        update();
    }
}

} // namespace fhmi
