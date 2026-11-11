#include "core/widget_meta.h"

#include "core/i18n.h"
#include "core/industrial_catalog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>

#include <algorithm>

namespace fhmi {

QString categoryTitle(WidgetCategory category)
{
    switch (category) {
    case WidgetCategory::Basic:
        return QCoreApplication::translate("fhmi", "基础控件");
    case WidgetCategory::Layout:
        return QCoreApplication::translate("fhmi", "布局容器");
    case WidgetCategory::Display:
        return QCoreApplication::translate("fhmi", "显示控件");
    case WidgetCategory::Chart:
        return QCoreApplication::translate("fhmi", "图表控件");
    case WidgetCategory::AutomotiveElectronics:
        return QCoreApplication::translate("fhmi", "汽车电子");
    case WidgetCategory::ElectronicsDiy:
        return QCoreApplication::translate("fhmi", "电子 DIY");
    case WidgetCategory::EmbeddedSystems:
        return QCoreApplication::translate("fhmi", "嵌入式");
    case WidgetCategory::IndustrialEquipment:
        return QCoreApplication::translate("fhmi", "工业设备");
    case WidgetCategory::IndustrialInstrument:
        return QCoreApplication::translate("fhmi", "工业仪表");
    case WidgetCategory::Comm:
        return QCoreApplication::translate("fhmi", "通信控件");
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

QJsonObject WidgetMeta::newWidgetProperties() const
{
    QJsonObject props = defaultProperties();
    for (const PropDef &def : properties) {
        if ((def.type == PropType::String || def.type == PropType::Text) && def.defaultValue.isString()) {
            props.insert(def.name, translated(def.defaultValue.toString()));
        }
    }
    if (typeName == QLatin1String("RealtimeChart")) { // a curve without a name shows 曲线1 whatever the language
        props.insert(QStringLiteral("_curve_0_name"), QCoreApplication::translate("fhmi", "曲线%1").arg(1));
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
    if (isIndustrialCategory(category)) {
        return QStringLiteral("value");
    }
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
    const auto *meta = WidgetRegistry::instance().find(typeName);
    if (meta && isIndustrialCategory(meta->category) && meta->prop(QStringLiteral("minimum")) && meta->prop(QStringLiteral("maximum"))) {
        const auto defaults = meta->defaultProperties();
        double lo = props.value(QStringLiteral("minimum")).toDouble(defaults.value(QStringLiteral("minimum")).toDouble());
        double hi = props.value(QStringLiteral("maximum")).toDouble(defaults.value(QStringLiteral("maximum")).toDouble());
        if (lo > hi) {
            if (changed == QLatin1String("maximum")) {
                lo = hi;
                props.insert(QStringLiteral("minimum"), lo);
            } else {
                hi = lo;
                props.insert(QStringLiteral("maximum"), hi);
            }
        }
        if (props.contains(QStringLiteral("value"))) {
            const double value = props.value(QStringLiteral("value")).toDouble(defaults.value(QStringLiteral("value")).toDouble());
            props.insert(QStringLiteral("value"), std::clamp(value, lo, hi));
        }
        return props; // omitted properties keep their catalog defaults and stay omitted in the saved project
    }
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

} // namespace fhmi
