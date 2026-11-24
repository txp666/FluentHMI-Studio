#pragma once

#include <QtCore/QHash>
#include <QtWidgets/QWidget>

class QLineEdit;
class QPoint;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace fhmi {

class CanvasItem;
class DesignScene;

// Hierarchical view of every widget on the design canvas, including children inside layout
// containers and inactive TabWidget pages. Lets users select, reorder, move out of containers,
// and switch tab pages even when layout-managed children cannot be dragged on the canvas.
class WidgetOutline : public QWidget
{
    Q_OBJECT

  public:
    explicit WidgetOutline(QWidget *parent = nullptr);

    void setScene(DesignScene *scene);
    void rebuild();
    void syncSelection();

  signals:
    void itemContextMenuRequested(fhmi::CanvasItem *item, const QPoint &globalPos);

  private:
    QTreeWidgetItem *addNode(CanvasItem *item, QTreeWidgetItem *parent);
    void filter(const QString &text);
    void updateActionButtons();
    CanvasItem *selectedOutlineItem() const;
    void revealItem(CanvasItem *item);
    void moveSelectedBy(int delta);
    void moveSelectedOut();

    DesignScene *m_scene = nullptr;
    QLineEdit *m_search = nullptr;
    QTreeWidget *m_tree = nullptr;
    QToolButton *m_up = nullptr;
    QToolButton *m_down = nullptr;
    QToolButton *m_pullOut = nullptr;
    QHash<CanvasItem *, QTreeWidgetItem *> m_nodes;
    bool m_syncing = false;
};

} // namespace fhmi
