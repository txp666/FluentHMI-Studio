#pragma once

#include "core/project.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtWidgets/QWidget>

class QAction;
class QGraphicsView;

namespace fhmi {

class CanvasItem;
class DesignScene;
class PropertyEditor;
class WidgetOutline;
class WidgetPalette;

class DesignerPage : public QWidget
{
    Q_OBJECT

  public:
    explicit DesignerPage(QWidget *parent = nullptr);

    // The project with the canvas contents written into it.
    Project snapshot() const;
    void rememberEndpoint(const QString &connectionId, const QString &endpoint);
    void rememberTextSample(const QString &connectionId, const Message &message);
    void addPointFromPreview(const QJsonObject &binding, const QString &name);
    bool isModified() const { return m_modified; }
    QString displayName() const;

    // Offers to save unsaved changes; false means the user cancelled (or saving failed).
    bool maybeSave();
    bool newProject();
    bool openProject(const QString &path = QString());
    bool save();
    bool saveAs();

    void undo();
    void redo();
    bool canUndo() const { return m_historyIndex > 0; }
    bool canRedo() const { return m_historyIndex + 1 < m_history.size(); }
    // The selected widgets onto the clipboard, in the format PyQt UI Designer shares.
    void copy();
    // The clipboard's widgets into the selected container, or onto the page.
    void paste();

  signals:
    // Anything about the design changed; when `keepPreview` is true (e.g. saving a data point from
    // a running preview), active hardware connections remain open until the user stops preview.
    void projectChanged(bool keepPreview = false);
    void titleChanged(const QString &title);
    void notify(const QString &title, const QString &content, bool error);
    void warning(const QString &title, const QString &content);

  private:
    void buildUi();
    void load(const Project &project);
    bool saveTo(const QString &path);
    void setModified(bool modified);
    // Makes the design as it is now an undo step, or folds it into the last one when `mergeKey` (not empty)
    // is the one that step was recorded with.
    void recordChange(const QString &mergeKey = QString());
    void resetHistory();
    void restore(int index);
    QHash<QString, int> editorPages() const;
    void updateEditActions();
    void applyProjectSettings();
    void zoomBy(qreal factor);
    // Shows the whole page, never larger than 100 %.
    void fitCanvas();
    void showItemContextMenu(CanvasItem *target, const QPoint &globalPos);

    Project m_project;
    bool m_modified = false;
    bool m_keepPreviewOnNextChange = false;
    // Each undo step remembers the design and the view of its tab containers.
    struct HistoryStep
    {
        QJsonObject project;
        QHash<QString, int> editorPages;
    };
    QList<HistoryStep> m_history;
    int m_historyIndex = 0;
    int m_savedIndex = 0; // -1 once the saved design has left the history
    QString m_lastMergeKey;
    // Pasting the same content into the same place again moves it one more grid step.
    QByteArray m_pasteContent;
    QString m_pasteTarget;
    int m_pasteCount = 0;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_copyAction = nullptr;
    DesignScene *m_scene = nullptr;
    QGraphicsView *m_view = nullptr;
    WidgetPalette *m_palette = nullptr;
    WidgetOutline *m_outline = nullptr;
    PropertyEditor *m_properties = nullptr;
};

} // namespace fhmi
