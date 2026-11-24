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
    m_properties = normalizedWidgetProperties(typeName, properties);
    if (typeName == QLatin1String("TabWidget"))
        m_editorPage = clampTabIndex(propertyValue(QStringLiteral("currentTab")).toInt(0),
                                    int(tabNames(propertyValue(QStringLiteral("tabNames")).toString()).size()));
    m_size = QSizeF(m_meta->defaultWidth, m_meta->defaultHeight);

    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);

    m_widget = createHmiWidget(typeName, m_properties, nullptr, assetBaseDir);
    m_size = m_widget->size();
    if (isVerticalSeparator())
        m_size.transpose();
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
    const qreal height = geo.value(QStringLiteral("h")).toDouble(item->m_size.height());
    // Measure the rows at the saved width; a wide S7 card needs fewer rows than the default narrow one.
    item->setSize(QSizeF(geo.value(QStringLiteral("w")).toDouble(item->m_size.width()), height), false);
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
    const bool fitHeight =
        isLinkCard() && (name == QLatin1String("displayMode") || qAbs(m_size.height() - minimumHeight()) < 1);
    const bool tabs = typeName() == QLatin1String("TabWidget");
    const bool navigationChanged = tabs &&
                                   name == QLatin1String("navigationStyle") && value != propertyValue(name);
    const bool separatorWasVertical = isVerticalSeparator();
    const int oldContentTop = navigationChanged ? m_widget->contentTop() : 0;
    QJsonObject props = m_properties;
    props.insert(name, value);
    if (tabs) {
        const QString names = props.value(QStringLiteral("tabNames"))
                                  .toString(propertyValue(QStringLiteral("tabNames")).toString());
        const int count = int(tabNames(names).size());
        if (props.contains(QStringLiteral("currentTab")))
            props.insert(QStringLiteral("currentTab"),
                         clampTabIndex(props.value(QStringLiteral("currentTab")).toInt(0), count));
        m_editorPage = clampTabIndex(m_editorPage, count);
        if (name == QLatin1String("tabNames")) {
            // Children on removed pages move to the last page.
            for (CanvasItem *child : std::as_const(m_children)) {
                child->m_tabIndex = clampTabIndex(child->m_tabIndex, count);
            }
        }
    }
    m_properties = normalizedWidgetProperties(typeName(), props, name);
    applyWidgetProperties();
    if (separatorWasVertical != isVerticalSeparator()) {
        setSize(m_size.transposed());
        if (m_containerParent)
            m_containerParent->relayoutChildren();
    }
    if (navigationChanged) {
        const int offset = m_widget->contentTop() - oldContentTop;
        for (CanvasItem *child : std::as_const(m_children)) {
            const QString layout = m_properties.value(QStringLiteral("_tabLayout_%1").arg(child->m_tabIndex))
                                       .toString(QStringLiteral("自由")); // i18n: value
            if (layout == QStringLiteral("自由")) {                       // i18n: value
                child->moveBy(0, offset);
            }
        }
    }
    if (navigationChanged || (tabs && name == QLatin1String("tabNames"))) {
        relayoutPages();
    } else if (isContainer() && !(tabs && name == QLatin1String("currentTab"))) {
        relayoutChildren();
    }
    if (isLinkCard() && (fitHeight || m_size.height() < minimumHeight())) {
        setSize(QSizeF(m_size.width(), minimumHeight()));
    }
    update();
}

void CanvasItem::applyWidgetProperties()
{
    QJsonObject view = m_properties;
    if (typeName() == QLatin1String("TabWidget"))
        view.insert(QStringLiteral("currentTab"), m_editorPage);
    m_widget->setProperties(view);
}

void CanvasItem::setEditorPage(int index)
{
    if (typeName() != QLatin1String("TabWidget"))
        return;
    const int count = int(tabNames(propertyValue(QStringLiteral("tabNames")).toString()).size());
    index = clampTabIndex(index, count);
    if (m_editorPage == index)
        return;
    m_editorPage = index;
    applyWidgetProperties();
    updatePageVisibility();
    emit editorPageChanged();
}

void CanvasItem::updatePages(const QStringList &names, int startup, int editor)
{
    m_properties.insert(QStringLiteral("tabNames"), names.join(QLatin1Char('\n')));
    if (startup != 0 || m_properties.contains(QStringLiteral("currentTab")))
        m_properties.insert(QStringLiteral("currentTab"), clampTabIndex(startup, int(names.size())));
    m_editorPage = clampTabIndex(editor, int(names.size()));
    applyWidgetProperties();
    relayoutPages();
    update();
}

void CanvasItem::relayoutPages()
{
    // Geometry edits affect all pages. Browsing only changes which children are visible.
    updatePageVisibility();
    const int count = int(tabNames(propertyValue(QStringLiteral("tabNames")).toString()).size());
    for (int page = 0; page < count; ++page) {
        QList<CanvasItem *> children;
        for (CanvasItem *child : std::as_const(m_children)) {
            if (child->m_tabIndex == page)
                children.append(child);
        }
        arrangeChildren(children, m_properties.value(QStringLiteral("_tabLayout_%1").arg(page))
                                      .toString(QStringLiteral("自由"))); // i18n: value
    }
}

int CanvasItem::addPage(const QString &name)
{
    if (typeName() != QLatin1String("TabWidget") || name.contains(QLatin1Char('\n'))) {
        return -1;
    }
    QStringList names = tabNames(propertyValue(QStringLiteral("tabNames")).toString());
    names.append(name.trimmed().isEmpty() ? tr("页面%1").arg(names.size() + 1) : name.trimmed());
    const int index = int(names.size()) - 1;
    // A page added after an older bulk edit starts with its own default layout.
    m_properties.remove(QStringLiteral("_tabLayout_%1").arg(index));
    updatePages(names, propertyValue(QStringLiteral("currentTab")).toInt(0), index);
    return index;
}

bool CanvasItem::removePage(int index)
{
    if (typeName() != QLatin1String("TabWidget")) {
        return false;
    }
    QStringList names = tabNames(propertyValue(QStringLiteral("tabNames")).toString());
    const int count = int(names.size());
    if (count < 2 || index < 0 || index >= count) {
        return false;
    }
    const int destination = std::min(index, count - 2);
    for (CanvasItem *child : std::as_const(m_children)) {
        if (child->m_tabIndex == index) {
            child->m_tabIndex = destination;
        } else if (child->m_tabIndex > index) {
            --child->m_tabIndex;
        }
    }
    for (int page = index; page < count - 1; ++page) {
        const QString key = QStringLiteral("_tabLayout_%1").arg(page);
        const QJsonValue next = m_properties.value(QStringLiteral("_tabLayout_%1").arg(page + 1));
        if (next.isUndefined()) {
            m_properties.remove(key);
        } else {
            m_properties.insert(key, next);
        }
    }
    m_properties.remove(QStringLiteral("_tabLayout_%1").arg(count - 1));
    const auto remainingIndex = [index, destination](int page) {
        if (page == index)
            return destination;
        return page > index ? page - 1 : page;
    };
    names.removeAt(index);
    updatePages(names, remainingIndex(propertyValue(QStringLiteral("currentTab")).toInt(0)),
                remainingIndex(m_editorPage));
    return true;
}

bool CanvasItem::movePage(int from, int to)
{
    if (typeName() != QLatin1String("TabWidget")) {
        return false;
    }
    QStringList names = tabNames(propertyValue(QStringLiteral("tabNames")).toString());
    const int count = int(names.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return false;
    }
    const auto movedIndex = [from, to](int index) {
        if (index == from)
            return to;
        if (from < to && index > from && index <= to)
            return index - 1;
        if (to < from && index >= to && index < from)
            return index + 1;
        return index;
    };
    const QJsonObject previous = m_properties;
    for (int page = 0; page < count; ++page) {
        const QString key = QStringLiteral("_tabLayout_%1").arg(movedIndex(page));
        const QJsonValue layout = previous.value(QStringLiteral("_tabLayout_%1").arg(page));
        if (layout.isUndefined()) {
            m_properties.remove(key);
        } else {
            m_properties.insert(key, layout);
        }
    }
    for (CanvasItem *child : std::as_const(m_children)) {
        child->m_tabIndex = movedIndex(child->m_tabIndex);
    }
    names.move(from, to);
    updatePages(names, movedIndex(propertyValue(QStringLiteral("currentTab")).toInt(0)), movedIndex(m_editorPage));
    return true;
}

bool CanvasItem::renamePage(int index, const QString &name)
{
    if (typeName() != QLatin1String("TabWidget")) {
        return false;
    }
    QStringList names = tabNames(propertyValue(QStringLiteral("tabNames")).toString());
    const QString trimmed = name.trimmed();
    if (index < 0 || index >= names.size() || trimmed.isEmpty() || trimmed.contains(QLatin1Char('\n')) ||
        trimmed == names.at(index)) {
        return false;
    }
    names[index] = trimmed;
    updatePages(names, propertyValue(QStringLiteral("currentTab")).toInt(0), m_editorPage);
    return true;
}

bool CanvasItem::isLinkCard() const
{
    return m_widget->fitsContentHeight();
}

qreal CanvasItem::rowsHeight() const
{
    return m_widget->minimumSizeHint().height();
}

bool CanvasItem::isVerticalSeparator() const
{
    return typeName() == QLatin1String("Separator") &&
           propertyValue(QStringLiteral("orientation")).toString() == QLatin1String("vertical");
}

qreal CanvasItem::minimumWidth() const
{
    return isVerticalSeparator() ? m_meta->minHeight : m_meta->minWidth;
}

qreal CanvasItem::minimumHeight() const
{
    const qreal height = isVerticalSeparator() ? m_meta->minWidth : m_meta->minHeight;
    return isLinkCard() ? std::max(height, rowsHeight()) : height;
}

void CanvasItem::setSize(const QSizeF &size, bool enforceMinimum)
{
    const bool linkCard = isLinkCard();
    // Link cards measure their component rows at the requested width, which may differ from the current one.
    const qreal minHeight = linkCard ? m_meta->minHeight : minimumHeight();
    QSizeF bounded(std::max<qreal>(size.width(), enforceMinimum ? minimumWidth() : 1),
                   std::max<qreal>(size.height(), enforceMinimum ? minHeight : 1));
    if (linkCard) {
        bounded.setHeight(std::max<qreal>(bounded.height(), m_widget->sizeForGeometry(bounded.toSize()).height()));
    }
    if (bounded == m_size) {
        return;
    }
    prepareGeometryChange();
    m_size = bounded;
    syncWidget();
    if (typeName() == QLatin1String("TabWidget")) {
        relayoutPages();
    } else if (isContainer()) {
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
    if (widthManagedByLayout())
        return {{Top, at(r.center().x(), r.top())}, {Bottom, at(r.center().x(), r.bottom())}};
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
    const qreal minW = minimumWidth();
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
        emit changed(this, wasResizing);
    } else if (!wasResizing && event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier &&
               typeName() == QLatin1String("TabWidget")) {
        const int page = static_cast<TabContainerWidget *>(m_widget)->navigationPageAt(event->pos().toPoint());
        if (page >= 0)
            setEditorPage(page);
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
        // Qt paints SizeAllCursor from an image, and on macOS 27 with Qt 6.11 that image's conversion
        // traps inside Qt (QImage::toCGImage), so the native hand cursor marks a movable item there.
#ifdef Q_OS_MACOS
        const Qt::CursorShape movable = Qt::OpenHandCursor;
#else
        const Qt::CursorShape movable = Qt::SizeAllCursor;
#endif
        setCursor((flags() & ItemIsMovable) ? movable : Qt::ArrowCursor);
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
        child->m_tabIndex = clampTabIndex(tabIndex >= 0 ? tabIndex : m_editorPage, pages);
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

QString CanvasItem::layoutMode(int page) const
{
    const QString type = typeName();
    if (type == QLatin1String("HBoxLayout")) {
        return QStringLiteral("水平"); // i18n: value
    }
    if (type == QLatin1String("VBoxLayout") || type == QLatin1String("GroupBox")) {
        return QStringLiteral("垂直"); // i18n: value
    }
    if (type == QLatin1String("FlexLayout")) {
        return propertyValue(QStringLiteral("direction")).toString() == QLatin1String("column")
                   ? QStringLiteral("弹性列")      // i18n: ignore
                   : QStringLiteral("弹性(Flex)"); // i18n: value
    }
    if (type == QLatin1String("CardContainer")) {
        return propertyValue(QStringLiteral("layout")).toString(QStringLiteral("自由")); // i18n: value
    }
    if (type == QLatin1String("TabWidget")) {
        return m_properties.value(QStringLiteral("_tabLayout_%1").arg(page >= 0 ? page : m_editorPage))
            .toString(QStringLiteral("自由")); // i18n: value
    }
    return QStringLiteral("自由"); // i18n: value
}

bool CanvasItem::isFreeLayout() const
{
    return layoutMode() == QStringLiteral("自由"); // i18n: value
}

bool CanvasItem::widthManagedByLayout() const
{
    return m_containerParent && m_containerParent->layoutMode(m_tabIndex) == QStringLiteral("垂直"); // i18n: value
}

void CanvasItem::fitWidth(qreal available)
{
    setSize(QSizeF(available, m_size.height()));
}

void CanvasItem::updatePageVisibility()
{
    const bool free = isFreeLayout();
    for (CanvasItem *child : std::as_const(m_children)) {
        const bool shown = child->m_tabIndex == m_editorPage;
        child->setVisible(shown);
        if (shown)
            child->setFlag(ItemIsMovable, free);
    }
}

void CanvasItem::relayoutChildren()
{
    const bool tabs = typeName() == QLatin1String("TabWidget");
    if (tabs)
        updatePageVisibility();
    QList<CanvasItem *> visible;
    for (CanvasItem *child : std::as_const(m_children)) {
        if (!tabs)
            child->setVisible(true);
        if (!tabs || child->m_tabIndex == m_editorPage)
            visible.append(child);
    }
    arrangeChildren(visible, layoutMode());
}

void CanvasItem::arrangeChildren(const QList<CanvasItem *> &visible, const QString &mode)
{
    const bool free = mode == QStringLiteral("自由"); // i18n: value
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

    if (mode == QStringLiteral("水平")) { // i18n: value
        qreal x = pad;
        for (CanvasItem *child : std::as_const(visible)) {
            child->setPos(x, top);
            x += child->m_size.width() + spacing;
        }
    } else if (mode == QStringLiteral("垂直")) { // i18n: value
        qreal y = top;
        for (CanvasItem *child : std::as_const(visible)) {
            child->setPos(pad, y);
            child->fitWidth(width - 2 * pad);
            y += child->m_size.height() + spacing;
        }
    } else if (mode == QStringLiteral("弹性(Flex)")) { // i18n: value
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
    } else if (mode == QStringLiteral("弹性列")) { // i18n: ignore
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
