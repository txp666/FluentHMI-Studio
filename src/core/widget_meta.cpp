#include "core/widget_meta.h"

#include <QtCore/QHash>

#include <algorithm>

namespace fhmi {

QString categoryTitle(WidgetCategory category)
{
    switch (category) {
    case WidgetCategory::Basic:
        return QStringLiteral("基础控件");
    case WidgetCategory::Layout:
        return QStringLiteral("布局容器");
    case WidgetCategory::Display:
        return QStringLiteral("显示控件");
    case WidgetCategory::Chart:
        return QStringLiteral("图表控件");
    case WidgetCategory::Comm:
        return QStringLiteral("通信控件");
    case WidgetCategory::Composite:
        return QStringLiteral("复合卡片");
    }
    return QString();
}

QJsonObject WidgetMeta::defaultProperties() const
{
    QJsonObject props;
    for (const PropDef &def : properties) {
        props.insert(def.name, def.defaultValue);
    }
    return props;
}

const PropDef *WidgetMeta::prop(const QString &name) const
{
    for (const PropDef &def : properties) {
        if (def.name == name) {
            return &def;
        }
    }
    return nullptr;
}

QString WidgetMeta::bindProperty() const
{
    static const QHash<QString, QString> mapping = {
        {QStringLiteral("Label"), QStringLiteral("text")},
        {QStringLiteral("LEDIndicator"), QStringLiteral("on")},
        {QStringLiteral("NumericDisplay"), QStringLiteral("value")},
        {QStringLiteral("ProgressBar"), QStringLiteral("value")},
        {QStringLiteral("ProgressRingDisplay"), QStringLiteral("value")},
        {QStringLiteral("GaugeMeter"), QStringLiteral("value")},
        {QStringLiteral("ToggleSwitch"), QStringLiteral("checked")},
        {QStringLiteral("Slider"), QStringLiteral("value")},
        {QStringLiteral("SpinBox"), QStringLiteral("value")},
    };
    return mapping.value(typeName);
}

const WidgetRegistry &WidgetRegistry::instance()
{
    static const WidgetRegistry registry;
    return registry;
}

WidgetRegistry::WidgetRegistry() : m_widgets(builtinWidgets()) {}

const WidgetMeta *WidgetRegistry::find(const QString &typeName) const
{
    for (const WidgetMeta &meta : m_widgets) {
        if (meta.typeName == typeName) {
            return &meta;
        }
    }
    return nullptr;
}

QList<const WidgetMeta *> WidgetRegistry::byCategory(WidgetCategory category) const
{
    QList<const WidgetMeta *> out;
    for (const WidgetMeta &meta : m_widgets) {
        if (meta.category == category) {
            out.append(&meta);
        }
    }
    return out;
}

QList<WidgetCategory> WidgetRegistry::categories() const
{
    QList<WidgetCategory> out;
    for (const WidgetMeta &meta : m_widgets) {
        if (!out.contains(meta.category)) {
            out.append(meta.category);
        }
    }
    return out;
}

QJsonObject normalizedRangeProperties(const QString &typeName, QJsonObject props, const QString &changed)
{
    const bool isGauge = typeName == QLatin1String("GaugeMeter");
    if (typeName != QLatin1String("Slider") && typeName != QLatin1String("SpinBox") && !isGauge) {
        return props;
    }
    double lo = props.value(QStringLiteral("minimum")).toDouble(0);
    double hi = props.value(QStringLiteral("maximum")).toDouble(100);
    if (lo > hi) {
        if (changed == QLatin1String("maximum")) {
            lo = hi;
        } else {
            hi = lo;
        }
    }
    const double fallback = typeName == QLatin1String("Slider") ? 50 : 0;
    const double value = std::clamp(props.value(QStringLiteral("value")).toDouble(fallback), lo, hi);
    if (isGauge) {
        props.insert(QStringLiteral("minimum"), lo);
        props.insert(QStringLiteral("maximum"), hi);
        props.insert(QStringLiteral("value"), value);
    } else {
        props.insert(QStringLiteral("minimum"), qRound64(lo));
        props.insert(QStringLiteral("maximum"), qRound64(hi));
        props.insert(QStringLiteral("value"), qRound64(value));
    }
    return props;
}

QJsonObject upgradedProperties(const QString &typeName, QJsonObject props)
{
    if (typeName == QLatin1String("ModbusConfigCard") && !props.contains(QStringLiteral("pollMode")) &&
        !props.value(QStringLiteral("pollList")).toString().isEmpty()) {
        props.insert(QStringLiteral("pollMode"), QStringLiteral("轮询表"));
    }
    return props;
}

} // namespace fhmi
