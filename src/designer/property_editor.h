#pragma once

#include "core/project.h"
#include "core/widget_meta.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <functional>

class QFormLayout;
class QToolButton;
class QVBoxLayout;

namespace FluentQt {
class ComboBox;
class SpinBox;
} // namespace FluentQt

namespace fhmi {

class CanvasItem;
class DesignScene;
struct FrameSource;
struct ReceivedField;

// Edits the selected canvas item (ID, geometry, properties, data binding), or the
// project itself when nothing is selected.
class PropertyEditor : public FluentQt::ScrollArea
{
    Q_OBJECT

  public:
    explicit PropertyEditor(QWidget *parent = nullptr);

    void setScene(DesignScene *scene) { m_scene = scene; }
    void setProject(Project *project);
    void editProjectLogic();
    void showItem(CanvasItem *item);
    void showBinding(CanvasItem *item)
    {
        m_bindingOnly = true;
        showItem(item);
    }
    CanvasItem *currentItem() const { return m_item; }
    // Reload the geometry fields after the item moved on the canvas.
    void refreshGeometry();

  signals:
    // A design change. Changes with the same key in a row (typing in one field, a spin box's arrows) are one
    // undo step; an empty key always starts a new one.
    void projectSettingsChanged(const QString &mergeKey);
    void modified(const QString &mergeKey);
    void message(const QString &title, const QString &content, bool error);

  private:
    enum class ReceiveTarget { Defaults, Connection, Binding, Curve, Observer };
    using Commit = std::function<void(const QJsonValue &)>;
    using ConfigRead = std::function<QJsonObject()>;
    // The edited config, and the key of the row that changed it.
    using ConfigWrite = std::function<void(const QJsonObject &, const QString &key)>;

    void rebuild();
    void clearContent();
    QFormLayout *addSection(const QString &title);
    void addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *editor,
                const QString &helpKey = {});
    QWidget *createEditor(const PropDef &def, const QJsonValue &value, const Commit &commit);

    void buildProjectForm();
    void buildBitProfilesForm();
    // The project's icon beside its name: choose an image or go back to the default.
    QWidget *createIconButton(QWidget *nameField);
    void buildItemForm();
    void buildItemProperties();
    void addItemProperties(QFormLayout *form, const QList<const PropDef *> &properties);
    void editPropertyRows(const PropDef &def);
    void addS7Rows(QFormLayout *form, const PropDef &def);
    void addPageRows(QFormLayout *form);
    void buildBindingForm(QFormLayout *form);
    void addCanBindingRow(QFormLayout *form, bool curve);
    void updateCanBindingRow(const QJsonObject &binding, bool curve);
    void showSendPreview();
    void editFrameTemplate();
    void editTransmitFormat();
    void updateDefaultFrameTemplate(const QJsonObject &previous);
    // The parserConfig rows, the same for the project's default frame format and a link card's
    // own; `prefix` keeps the card's row keys apart from its properties'. Every edit hands
    // `write` the whole config `read` returns, with that one key changed.
    void addFrameRows(QFormLayout *form, const QString &prefix, const ConfigRead &read, const ConfigWrite &write);
    void addReceivePreviewRow(QFormLayout *form, const QString &key);
    void addReceiveEditorRow(QFormLayout *form, const QString &key, ReceiveTarget target);
    void addReceiveTemplateRow(QFormLayout *form, const QString &key, ReceiveTarget target);
    void addTemplateChoiceRow(QFormLayout *form, const QString &key, const QString &label,
                              const std::function<void(const QString &)> &choose, const std::function<void()> &manage,
                              const std::function<void()> &chooseGroup = {});
    bool updateReceiveTemplateRow(const QString &key, ReceiveTarget target, bool active = true);
    bool updateTemplateFieldPicker(const QString &row, ReceiveTarget target, FluentQt::ComboBox *mode,
                                   const QString &parseMode, const QString &currentField);
    void manageFrameTemplates(const LinkParser::Format &seed, const QString &selected);
    void setReceiveTemplate(ReceiveTarget target, const QString &id);
    bool setReceiveTemplates(ReceiveTarget target, const QStringList &ids);
    FrameSource receiveSource(ReceiveTarget target) const;
    void bindReceiveField(ReceiveTarget target, const ReceivedField &field, const QString &sourceId);
    void openReceiveEditor(ReceiveTarget target, QWidget *anchor);
    void applyProjectDraft(const Project &draft);
    void addDataPoint(QJsonObject binding, const QString &suggestedName, CanvasItem *boundItem = nullptr);
    void setFrameRowsVisible(const QString &prefix, const QString &mode);
    // The frame format a link card's rows show: its own, or the project's while its own is
    // empty and the modes match, as the card frames at run time.
    QJsonObject cardFrameConfig() const;

    // Where a property is stored: chart curve fields and tab page layouts live in
    // per-curve / per-page keys, exactly as PyQt UI Designer stores them.
    QString storageKey(const QString &name) const;
    QJsonValue itemValue(const PropDef &def) const;
    // `detail` tells apart edits of one property that are not one step (the rows of a card's frame format).
    void commitProperty(const QString &name, const QJsonValue &value, const QString &detail = QString());
    void commitBinding(const QString &field);
    void refreshNumericRows();
    void updateVisibility();
    void setRowVisible(const QString &key, bool visible);
    void setModbusReadFields(const QJsonObject &binding, const QString &typeRow, const QString &orderRow);
    void updateRouteEndpoint(const QString &connection, const QString &endpoint, bool active = true);

    QPointer<CanvasItem> m_item;
    DesignScene *m_scene = nullptr;
    Project *m_project = nullptr;
    QString m_bitProfileName = QStringLiteral("default");
    QWidget *m_content = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QHash<QString, QPair<QWidget *, QWidget *>> m_rows;
    QHash<QString, QToolButton *> m_helpButtons;
    QHash<QString, QWidget *> m_bindingEditors;
    FluentQt::SpinBox *m_geometry[4] = {};
    bool m_loading = false;
    bool m_bindingOnly = false;
};

} // namespace fhmi
