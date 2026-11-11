#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace fhmi {

enum class PropType
{
    String,
    Int,
    Float,
    Bool,
    Color,
    Enum,
    Text,    // multi-line string
    Command, // a command string sent to the device
    Object,  // a JSON object with a section of its own in the property editor, not one row
};

// The labels, display names and default texts here are Chinese source texts (QT_TR_NOOP): translate them where
// they are shown, with translated() from core/i18n.h. Enum options are .uidp values and never change; only their
// display is translated.
struct PropDef
{
    QString name;
    QString label;
    PropType type = PropType::String;
    QJsonValue defaultValue;
    QStringList options; // Enum only
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool bindable = false;
    QString description; // for the widget reference, where a label alone does not explain it
};

enum class WidgetCategory
{
    Basic,
    Layout,
    Display,
    Chart,
    Comm,
    IndustrialEquipment,
    IndustrialInstrument,
    AutomotiveElectronics,
    ElectronicsDiy,
    EmbeddedSystems,
};

// The category's title, translated.
QString categoryTitle(WidgetCategory category);

struct WidgetMeta
{
    QString typeName;
    QString displayName;
    WidgetCategory category = WidgetCategory::Basic;
    QString icon; // a FluentIcon name or one of the palette's own, see widgetIcon(): "send", "button"
    int defaultWidth = 120;
    int defaultHeight = 40;
    QList<PropDef> properties;
    bool resizable = true;
    int minWidth = 30;
    int minHeight = 20;
    bool container = false;

    // What a property missing from a file reads as: the same whatever the language.
    QJsonObject defaultProperties() const;
    // The properties of a widget added from the palette: the defaults, with its default texts (a button's 按钮,
    // a chart's title) in the interface language. They are all written to the file, so it opens the same anywhere.
    QJsonObject newWidgetProperties() const;
    const PropDef *prop(const QString &name) const;
    // The property a data binding writes into, or empty if the widget cannot be bound.
    QString bindProperty() const;
};

class WidgetRegistry
{
  public:
    static const WidgetRegistry &instance();

    const WidgetMeta *find(const QString &typeName) const;
    const QList<WidgetMeta> &all() const { return m_widgets; }
    QList<const WidgetMeta *> byCategory(WidgetCategory category) const;
    QList<WidgetCategory> categories() const;

  private:
    WidgetRegistry();

    QList<WidgetMeta> m_widgets;
};

// Keep the range endpoints and the initial value of Slider/SpinBox/GaugeMeter consistent.
// `changed` names the property the user just edited, so that endpoint wins.
QJsonObject normalizedRangeProperties(const QString &typeName, QJsonObject properties,
                                      const QString &changed = QString());

// Registered in builtin_widgets.cpp.
QList<WidgetMeta> builtinWidgets();

} // namespace fhmi
