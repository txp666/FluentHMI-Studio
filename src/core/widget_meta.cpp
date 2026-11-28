#include "core/widget_meta.h"

#include "core/binding.h"
#include "core/command_encoder.h"
#include "core/i18n.h"
#include "core/industrial_catalog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>

#include <algorithm>
#include <cmath>

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
        } else if (def.defaultValue.isArray() && std::any_of(def.columns.cbegin(), def.columns.cend(), [](const auto &column) {
                       return column.first == QLatin1String("label");
                   })) {
            QJsonArray rows;
            for (const auto &entry : def.defaultValue.toArray()) {
                auto row = entry.toObject();
                const QString label = row.value(QStringLiteral("label")).toString();
                row.insert(QStringLiteral("label"), translated(label));
                rows.append(row);
            }
            props.insert(def.name, rows);
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
        {QStringLiteral("PushButton"), QStringLiteral("text")},
        {QStringLiteral("ComboBox"), QStringLiteral("currentIndex")},
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

QString commandActivationProperty(QStringView typeName)
{
    if (typeName == QLatin1String("LineEdit"))
        return QStringLiteral("inputOnly");
    if (typeName == QLatin1String("ComboBox"))
        return QStringLiteral("sendOnSelect");
    if (typeName == QLatin1String("Slider") || typeName == QLatin1String("SpinBox"))
        return QStringLiteral("sendOnChange");
    return {};
}

namespace {

bool lineEditInputOnly(const QJsonObject &properties)
{
    const auto inputOnly = properties.value(QStringLiteral("inputOnly"));
    if (!inputOnly.isUndefined())
        return inputOnly.toBool(inputOnly.toDouble() != 0);
    const auto readOnly = properties.value(QStringLiteral("readOnly"));
    if (readOnly.toBool(readOnly.toDouble() != 0))
        return true;
    const auto sendOnEnter = properties.value(QStringLiteral("sendOnEnter"));
    const auto mode = CommandEncoder::commandMode(properties.value(QStringLiteral("commandMode")).toString());
    // Legacy Modbus and S7 inputs sent on Return independently of the text-mode checkbox.
    return mode != CommandEncoder::Mode::Modbus && mode != CommandEncoder::Mode::S7 &&
           !sendOnEnter.toBool(sendOnEnter.toDouble() != 0);
}

} // namespace

bool widgetSendsCommands(QStringView typeName, const QJsonObject &properties)
{
    if (typeName == QLatin1String("LineEdit"))
        return properties.value(QStringLiteral("parameterName")).toString().isEmpty() && !lineEditInputOnly(properties);
    const QString activation = commandActivationProperty(typeName);
    if (activation.isEmpty())
        return true;
    const auto value = properties.value(activation);
    return value.toBool(value.toDouble() != 0);
}

bool widgetReadsStoredBinding(const WidgetMeta &meta, const QJsonObject &properties)
{
    if (isTimingChart(meta.typeName) && TimingSource::usesChannels(properties))
        return false;
    if (!meta.bindProperty().isEmpty())
        return true;
    const auto readOnly = properties.value(QStringLiteral("readOnly"));
    return meta.typeName == QLatin1String("LineEdit") && lineEditInputOnly(properties) &&
           readOnly.toBool(readOnly.toDouble() != 0) && properties.value(QStringLiteral("parameterName")).toString().isEmpty();
}

namespace {

bool integerSpinBox(const WidgetMeta &meta, const QJsonObject &props)
{
    return meta.typeName == QLatin1String("SpinBox") &&
           props.value(QStringLiteral("numberMode")).toString() != QStringLiteral("小数"); // i18n: value
}

double boundedNumber(const PropDef &definition, const QJsonObject &props)
{
    double number = props.value(definition.name).toDouble(definition.defaultValue.toDouble());
    if (definition.minimum)
        number = std::max(number, *definition.minimum);
    if (definition.maximum)
        number = std::min(number, *definition.maximum);
    return number;
}

std::optional<WidgetValueRange> configuredValueRange(const WidgetMeta &meta, const QJsonObject &props,
                                                    QStringView changed = {})
{
    const auto *minimum = meta.prop(QStringLiteral("minimum"));
    const auto *maximum = meta.prop(QStringLiteral("maximum"));
    if (!maximum || !meta.prop(QStringLiteral("value")))
        return std::nullopt;
    WidgetValueRange range{minimum ? boundedNumber(*minimum, props) : 0, boundedNumber(*maximum, props)};
    if (maximum->type == PropType::Int || integerSpinBox(meta, props)) {
        range.minimum = qRound64(range.minimum);
        range.maximum = qRound64(range.maximum);
    }
    if (range.minimum > range.maximum) {
        if (changed == QLatin1String("maximum"))
            range.minimum = range.maximum;
        else
            range.maximum = range.minimum;
    }
    return range;
}

void storeNumber(QJsonObject &props, const PropDef &definition, double value)
{
    // Omitted properties keep their metadata default unless another property changes their effective value.
    if (props.contains(definition.name) || definition.defaultValue != QJsonValue(value))
        props.insert(definition.name, value);
}

IndustrialCapabilities widgetIndustrialCapabilities(const WidgetMeta &meta, const QJsonObject &props)
{
    return isIndustrialCategory(meta.category)
               ? industrialCapabilities(meta.typeName, props.value(QStringLiteral("feedbackMode")).toString())
               : IndustrialCapabilities{};
}

} // namespace

std::optional<WidgetValueRange> widgetValueRange(const WidgetMeta &meta, const QJsonObject &props)
{
    const auto capabilities = widgetIndustrialCapabilities(meta, props);
    if (capabilities.sampleMaximum >= 0)
        return WidgetValueRange{0, double(capabilities.sampleMaximum)};
    return configuredValueRange(meta, props);
}

QJsonObject normalizedWidgetProperties(const QString &typeName, QJsonObject props, const QString &changed)
{
    // A JSON author can specify LF as an actual character. Store the same option the editor selects.
    const auto ending = props.value(QStringLiteral("commandNewline")).toString();
    if (ending == u"\n" || ending == u"\r" || ending == u"\r\n")
        props.insert(QStringLiteral("commandNewline"), QString(ending).replace(u'\r', QStringLiteral("\\r"))
                                                                      .replace(u'\n', QStringLiteral("\\n")));
    if (typeName == QLatin1String("LineEdit")) {
        props.insert(QStringLiteral("inputOnly"), lineEditInputOnly(props));
        props.remove(QStringLiteral("sendOnEnter"));
        if (!props.value(QStringLiteral("inputOnly")).toBool())
            props.remove(QStringLiteral("readOnly"));
    }
    if (typeName == QLatin1String("PushButton")) {
        const QString previous = QStringLiteral("mbWriteInputs");
        const QString current = QStringLiteral("commandInputs");
        if (props.contains(current))
            props.remove(previous);
        else if (props.contains(previous) && CommandEncoder::inputListMode(props) == CommandEncoder::InputListMode::Registers)
            props.insert(current, props.take(previous));
    }
    if (typeName == QLatin1String("CanMonitor") && !props.contains(QStringLiteral("autoSaveRows"))) {
        // Freeze the old shared limit once; later display-limit edits must not change recording.
        props.insert(QStringLiteral("autoSaveRows"),
                     std::clamp(props.value(QStringLiteral("maxMessages")).toInt(1000), 1, 50000));
    }
    const auto *meta = WidgetRegistry::instance().find(typeName);
    if (!meta)
        return props;
    const auto *valueDefinition = meta->prop(QStringLiteral("value"));
    const auto capabilities = widgetIndustrialCapabilities(*meta, props);
    if (capabilities.sampleMaximum >= 0) {
        const auto sample = props.value(QStringLiteral("value")).toDouble(capabilities.sampleDefault);
        const bool valid = sample >= 0 && sample <= capabilities.sampleMaximum && std::floor(sample) == sample;
        storeNumber(props, *valueDefinition, valid ? sample : capabilities.sampleDefault);
        return props;
    }
    if (const auto range = configuredValueRange(*meta, props, changed)) {
        if (const auto *minimum = meta->prop(QStringLiteral("minimum")))
            storeNumber(props, *minimum, range->minimum);
        storeNumber(props, *meta->prop(QStringLiteral("maximum")), range->maximum);
        double sample = props.value(valueDefinition->name).toDouble(valueDefinition->defaultValue.toDouble());
        if (valueDefinition->type == PropType::Int || integerSpinBox(*meta, props))
            sample = std::round(sample);
        storeNumber(props, *valueDefinition, std::clamp(sample, range->minimum, range->maximum));
    }
    if (typeName == QLatin1String("SpinBox")) {
        const auto &step = *meta->prop(QStringLiteral("singleStep"));
        const auto &decimals = *meta->prop(QStringLiteral("decimals"));
        const int precision = integerSpinBox(*meta, props) ? 0 : int(boundedNumber(decimals, props));
        const double scale = std::pow(10., precision);
        storeNumber(props, step, std::max(1., std::round(boundedNumber(step, props) * scale)) / scale);
    }
    return props;
}

} // namespace fhmi
