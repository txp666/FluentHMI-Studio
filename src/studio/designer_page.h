#pragma once

#include "core/project.h"

#include <QtWidgets/QWidget>

class QGraphicsView;

namespace fhmi {

class DesignScene;
class PropertyEditor;
class WidgetPalette;

class DesignerPage : public QWidget
{
    Q_OBJECT

  public:
    explicit DesignerPage(QWidget *parent = nullptr);

    // The project with the canvas contents written into it.
    Project snapshot() const;
    bool isModified() const { return m_modified; }
    QString displayName() const;

    // Offers to save unsaved changes; false means the user cancelled (or saving failed).
    bool maybeSave();
    bool newProject();
    bool openProject(const QString &path = QString());
    bool save();
    bool saveAs();

  signals:
    // Anything about the design changed; a running preview is now out of date.
    void projectChanged();
    void titleChanged(const QString &title);
    void notify(const QString &title, const QString &content, bool error);

  private:
    void buildUi();
    void load(const Project &project);
    bool saveTo(const QString &path);
    void setModified(bool modified);
    void applyProjectSettings();
    void zoomBy(qreal factor);
    // Shows the whole page, never larger than 100 %.
    void fitCanvas();

    Project m_project;
    bool m_modified = false;
    DesignScene *m_scene = nullptr;
    QGraphicsView *m_view = nullptr;
    WidgetPalette *m_palette = nullptr;
    PropertyEditor *m_properties = nullptr;
};

} // namespace fhmi
