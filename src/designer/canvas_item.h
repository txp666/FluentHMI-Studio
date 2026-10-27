#pragma once

#include "core/widget_meta.h"

#include <QtCore/QJsonObject>
#include <QtWidgets/QGraphicsObject>

class QGraphicsProxyWidget;

namespace fhmi {

class HmiWidget;

// One widget on the design canvas: the real widget, inert behind a graphics proxy,
// plus selection outline and resize handles. Container items hold their children as
// child graphics items, positioned relative to the container.
class CanvasItem : public QGraphicsObject
{
    Q_OBJECT

  public:
    enum
    {
        Type = UserType + 1
    };

    CanvasItem(const QString &typeName, const QJsonObject &properties, const QString &id,
               const QString &assetBaseDir = QString());
    ~CanvasItem() override;

    // Builds an item and its children from a project widget object.
    static CanvasItem *fromJson(const QJsonObject &data, const QString &assetBaseDir);
    QJsonObject toJson() const;

    int type() const override { return Type; }
    const WidgetMeta &meta() const { return *m_meta; }
    QString typeName() const { return m_meta->typeName; }
    HmiWidget *widget() const { return m_widget; }

    QString widgetId() const { return m_id; }
    void setWidgetId(const QString &id) { m_id = id; }

    QJsonObject properties() const { return m_properties; }
    QJsonValue propertyValue(const QString &name) const;
    void setPropertyValue(const QString &name, const QJsonValue &value);
    QJsonObject binding() const { return m_binding; }
    void setBinding(const QJsonObject &binding) { m_binding = binding; }

    int tabIndex() const { return m_tabIndex; }
    void setTabIndex(int index) { m_tabIndex = index; }

    QSizeF size() const { return m_size; }
    // User edits respect the widget's minimum size; loading keeps what the project says.
    void setSize(const QSizeF &size, bool enforceMinimum = true);
    QRectF rect() const { return QRectF(QPointF(), m_size); }
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

    void setGridSize(int size);
    void setAssetBaseDir(const QString &dir);

    // Containers
    bool isContainer() const { return m_meta->container; }
    CanvasItem *containerParent() const { return m_containerParent; }
    QList<CanvasItem *> containerChildren() const { return m_children; }
    bool isAncestorOf(const CanvasItem *item) const;
    // Adopts `child`, keeping its scene position unless `localPos` is given.
    void addChild(CanvasItem *child, int tabIndex = -1, const QPointF *localPos = nullptr);
    // Releases `child` to the scene's top level at its current scene position.
    void removeChild(CanvasItem *child);
    // Re-arranges children for layout containers and shows only the current tab page.
    void relayoutChildren();
    bool isFreeLayout() const;
    void setDropHighlight(bool on);

    void snapToGrid();

  signals:
    // The user moved or resized the item, or a property changed.
    void changed(fhmi::CanvasItem *item);

  protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

  private:
    enum Handle
    {
        None = 0,
        Left = 1,
        Top = 2,
        Right = 4,
        Bottom = 8,
    };
    int handleAt(const QPointF &pos) const;
    QList<QPair<int, QRectF>> handleRects() const;
    void syncWidget();
    QString layoutMode() const;
    void fitWidth(qreal available);

    const WidgetMeta *m_meta;
    QString m_id;
    QJsonObject m_properties;
    QJsonObject m_binding;
    int m_tabIndex = 0;
    QSizeF m_size;
    int m_gridSize = 10;
    QGraphicsProxyWidget *m_proxy = nullptr;
    HmiWidget *m_widget = nullptr;
    CanvasItem *m_containerParent = nullptr;
    QList<CanvasItem *> m_children;
    bool m_dropHighlight = false;

    int m_resizeHandle = None;
    QPointF m_pressScenePos;
    QPointF m_pressPos;
    QSizeF m_pressSize;
    bool m_moved = false;
};

} // namespace fhmi
