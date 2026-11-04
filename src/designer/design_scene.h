#pragma once

#include "core/project.h"

#include <QtWidgets/QGraphicsScene>

namespace fhmi {

class CanvasItem;

inline constexpr const char *WidgetMimeType = "application/x-fluenthmi-widget";
// Copied widgets on the clipboard, shared with PyQt UI Designer: {"version": "1.0", "widgets": [...]}.
inline constexpr const char *WidgetsClipboardType = "application/x-fluenthmi-widgets";

enum class Alignment
{
    Left,
    Right,
    Top,
    Bottom,
    HorizontalCenter,
    VerticalCenter,
    DistributeHorizontally,
    DistributeVertically,
};

// The design canvas: a fixed-size page with a snapping grid. Widgets dragged from the
// palette land here; dropping onto a container nests them.
class DesignScene : public QGraphicsScene
{
    Q_OBJECT

  public:
    explicit DesignScene(QObject *parent = nullptr);

    void setCanvasSize(const QSize &size);
    QSize canvasSize() const { return m_canvasSize; }
    void setGridSize(int size);
    int gridSize() const { return m_gridSize; }
    void setGridVisible(bool visible);
    bool isGridVisible() const { return m_gridVisible; }
    void setAssetBaseDir(const QString &dir);

    // Replaces the scene contents. Returns false (and keeps the old contents) if an
    // item cannot be built.
    bool loadWidgets(const QJsonArray &widgets, QString *error);
    QJsonArray widgetsJson() const;

    QList<CanvasItem *> topLevelItems() const;
    QList<CanvasItem *> allItems() const;
    QList<CanvasItem *> selectedCanvasItems() const;
    CanvasItem *findItem(const QString &id) const;
    void selectIds(const QStringList &ids);

    CanvasItem *createItem(const QString &typeName, const QPointF &scenePos);
    void deleteSelected();
    void clearAll();
    void align(Alignment alignment);
    QString uniqueId(const QString &typeName) const;
    bool renameItem(CanvasItem *item, const QString &id, QString *error);

    // The selected widgets as the clipboard carries them: a selected container takes its children, the
    // top-level ones have canvas positions and no tabIndex, and relative image paths become absolute.
    QJsonArray selectionJson() const;
    // Adds checked widgets, as copied, into `container` (its current page) with their bounding box at
    // (grid × step, grid × step), or at the top level shifted by that much; kept on the page (or in the
    // container) when they fit. Ids already in use are replaced. The pasted widgets become the selection.
    QList<CanvasItem *> paste(const QJsonArray &widgets, CanvasItem *container, int step);

    // Every ModbusSlaveTable shows the area of the first card in the document whose modbusRole is 从站, with its
    // slaveInit values. After each design change, and after loading.
    void refreshSlaveTables();

  signals:
    // A design change. Changes with the same non-empty key in a row (a run of arrow-key nudges) are one
    // undo step.
    void modified(const QString &mergeKey = QString());
    void itemPropertiesChanged(fhmi::CanvasItem *item);

  protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void dragEnterEvent(QGraphicsSceneDragDropEvent *event) override;
    void dragMoveEvent(QGraphicsSceneDragDropEvent *event) override;
    void dragLeaveEvent(QGraphicsSceneDragDropEvent *event) override;
    void dropEvent(QGraphicsSceneDragDropEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;

  private:
    void adopt(CanvasItem *item);
    // The innermost container under `scenePos`, ignoring `exclude` and its descendants.
    CanvasItem *containerAt(const QPointF &scenePos, const CanvasItem *exclude = nullptr) const;
    void highlightDropTarget(CanvasItem *target);
    void reparentAfterMove(CanvasItem *item);

    QSize m_canvasSize{1200, 800};
    int m_gridSize = 10;
    bool m_gridVisible = true;
    QString m_assetBaseDir;
    CanvasItem *m_dropTarget = nullptr;
};

} // namespace fhmi
