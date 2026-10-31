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
    Composite,
};

QString categoryTitle(WidgetCategory category);

struct WidgetMeta
{
    QString typeName;
    QString displayName;
    WidgetCategory category = WidgetCategory::Basic;
    QString icon; // FluentIcon name, e.g. "Send"
    int defaultWidth = 120;
    int defaultHeight = 40;
    QList<PropDef> properties;
    bool resizable = true;
    int minWidth = 30;
    int minHeight = 20;
    bool container = false;

    QJsonObject defaultProperties() const;
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

// Properties a project saved before they existed, filled in as that project meant them: a ModbusConfigCard
// without pollMode polls as 0.2.0 did (pollMode 轮询表, autoStart false). Project::fromJson() applies it on
// load; cards made in the designer get the defaults (自动, autoStart true) instead.
QJsonObject upgradedProperties(const QString &typeName, QJsonObject properties);

// Registered in builtin_widgets.cpp.
QList<WidgetMeta> builtinWidgets();

} // namespace fhmi
