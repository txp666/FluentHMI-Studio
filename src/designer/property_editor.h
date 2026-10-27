#pragma once

#include "core/project.h"
#include "core/widget_meta.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <functional>

class QFormLayout;
class QVBoxLayout;

namespace FluentQt {
class SpinBox;
} // namespace FluentQt

namespace fhmi {

class CanvasItem;
class DesignScene;

// Edits the selected canvas item (ID, geometry, properties, data binding), or the
// project itself when nothing is selected.
class PropertyEditor : public FluentQt::ScrollArea
{
    Q_OBJECT

  public:
    explicit PropertyEditor(QWidget *parent = nullptr);

    void setScene(DesignScene *scene) { m_scene = scene; }
    void setProject(Project *project);
    void showItem(CanvasItem *item);
    CanvasItem *currentItem() const { return m_item; }
    // Reload the geometry fields after the item moved on the canvas.
    void refreshGeometry();

  signals:
    void projectSettingsChanged();
    void modified();
    void message(const QString &title, const QString &content, bool error);

  private:
    using Commit = std::function<void(const QJsonValue &)>;

    void rebuild();
    void clearContent();
    QFormLayout *addSection(const QString &title);
    void addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *editor);
    QWidget *createEditor(const PropDef &def, const QJsonValue &value, const Commit &commit);

    void buildProjectForm();
    void buildItemForm();
    void buildBindingForm();

    // Where a property is stored: chart curve fields and tab page layouts live in
    // per-curve / per-page keys, exactly as PyQt UI Designer stores them.
    QString storageKey(const QString &name) const;
    QJsonValue itemValue(const PropDef &def) const;
    void commitProperty(const QString &name, const QJsonValue &value);
    void commitBinding();
    void updateVisibility();
    void setRowVisible(const QString &key, bool visible);

    QPointer<CanvasItem> m_item;
    DesignScene *m_scene = nullptr;
    Project *m_project = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QHash<QString, QPair<QWidget *, QWidget *>> m_rows;
    QHash<QString, QWidget *> m_bindingEditors;
    FluentQt::SpinBox *m_geometry[4] = {};
    bool m_loading = false;
};

} // namespace fhmi
