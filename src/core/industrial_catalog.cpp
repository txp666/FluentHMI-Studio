#include "core/industrial_catalog.h"
#include "core/i18n.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <algorithm>
#include <cmath>

static void initializeIndustrialResources() { Q_INIT_RESOURCE(industrial); }

namespace fhmi {
const QJsonArray &industrialCatalog()
{
    static const QJsonArray catalog = [] {
        initializeIndustrialResources();
        QFile file(QStringLiteral(":/fluenthmi/industrial/catalog.json"));
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("Industrial widget catalog is missing");
        }
        const auto document = QJsonDocument::fromJson(file.readAll());
        return document.isArray() ? document.array() : document.object().value(QStringLiteral("widgets")).toArray();
    }();
    return catalog;
}

QJsonObject industrialDefinition(const QString &type)
{
    static const auto definitions = [] {
        QHash<QString, QJsonObject> result;
        for (const auto &item : industrialCatalog()) {
            const auto definition = item.toObject();
            result.insert(definition.value(QStringLiteral("type")).toString(), definition);
        }
        return result;
    }();
    return definitions.value(type);
}

bool isIndustrialCategory(WidgetCategory category)
{
    return category == WidgetCategory::IndustrialEquipment || category == WidgetCategory::IndustrialInstrument ||
           category == WidgetCategory::AutomotiveElectronics || category == WidgetCategory::ElectronicsDiy ||
           category == WidgetCategory::EmbeddedSystems;
}

namespace {

IndustrialCapabilities capabilitiesOf(const QJsonObject &definition, QStringView feedbackMode)
{
    IndustrialCapabilities capabilities;
    const QString type = definition.value(QStringLiteral("type")).toString();
    const QString mode = definition.value(QStringLiteral("mode")).toString();
    capabilities.sampleDefault = definition.value(QStringLiteral("value")).toDouble();
    if (type == QLatin1String("Motor") && feedbackMode == QStringLiteral("运行位")) { // i18n: value
        capabilities.states = {translated(QT_TR_NOOP("停止")), translated(QT_TR_NOOP("运行"))};
        capabilities.sampleDefault = 0;
    } else if (type == QLatin1String("Cylinder") && feedbackMode == QStringLiteral("端位状态")) { // i18n: value
        capabilities.states = {translated(QT_TR_NOOP("两端未到位")), translated(QT_TR_NOOP("缩回")),
                               translated(QT_TR_NOOP("伸出"))};
        capabilities.sampleDefault = 0;
    } else if (mode == QLatin1String("state") || mode == QLatin1String("bits")) {
        const auto names = definition.value(language() == kLanguageEnglish ? QStringLiteral("statesEn")
                                                                           : QStringLiteral("states")).toArray();
        for (const auto &name : names)
            capabilities.states.append(name.toString());
    }
    if (!capabilities.states.isEmpty())
        capabilities.sampleMaximum = int(capabilities.states.size()) - 1;
    else if (mode == QLatin1String("byte") || mode == QLatin1String("flags"))
        capabilities.sampleMaximum = definition.value(QStringLiteral("maximum")).toInt();
    else if (mode == QLatin1String("word") || mode == QLatin1String("timing"))
        capabilities.sampleMaximum = 65535;

    capabilities.structuredInput = mode == QLatin1String("cells") || mode == QLatin1String("supply");
    if (mode == QLatin1String("cells"))
        capabilities.inputExample = QStringLiteral(R"({"cell_voltages":[3.692,3.701,3.687]})");
    else if (mode == QLatin1String("supply"))
        capabilities.inputExample = QStringLiteral(R"({"bench_supply":{"voltage":11.7,"current":1.0,"mode":"CC"}})");
    capabilities.numericReadout = mode == QLatin1String("number") && capabilities.states.isEmpty();
    if (capabilities.numericReadout) {
        const QString svg = definition.value(QStringLiteral("svg")).toString();
        const auto drivers = definition.value(QStringLiteral("slots")).toObject();
        capabilities.scale = definition.value(QStringLiteral("motion")).toString() == QLatin1String("signed") ||
                             svg.contains(QLatin1String("{{minLabel}}")) || svg.contains(QLatin1String("{{maxLabel}}")) ||
                             std::any_of(drivers.cbegin(), drivers.cend(), [](const QJsonValue &slot) {
                                 const QString driver = slot.toArray().at(0).toString();
                                 return driver == QLatin1String("level") || driver == QLatin1String("meniscus") ||
                                        driver == QLatin1String("partial") || driver == QLatin1String("magnitude") ||
                                        driver == QLatin1String("lowMark") || driver == QLatin1String("highMark") ||
                                        driver.startsWith(QLatin1String("seg"));
                             });
    }
    return capabilities;
}

} // namespace

IndustrialCapabilities industrialCapabilities(const QString &type, QStringView feedbackMode)
{
    return capabilitiesOf(industrialDefinition(type), feedbackMode);
}

bool industrialLimitsValid(double low, double high) { return std::isfinite(low) && std::isfinite(high) && low < high; }

QList<WidgetMeta> industrialWidgets()
{
    QList<WidgetMeta> result;
    for (const auto &item : industrialCatalog()) {
        const auto definition = item.toObject();
        const auto capabilities = capabilitiesOf(definition, {});
        WidgetMeta meta;
        meta.typeName = definition.value(QStringLiteral("type")).toString();
        const bool timing = meta.typeName == QLatin1String("DigitalTimingChart");
        meta.displayName = definition.value(QStringLiteral("name")).toString(meta.typeName);
        const QHash<QString, WidgetCategory> categories{
            {QStringLiteral("equipment"), WidgetCategory::IndustrialEquipment},
            {QStringLiteral("instrument"), WidgetCategory::IndustrialInstrument},
            {QStringLiteral("automotive"), WidgetCategory::AutomotiveElectronics},
            {QStringLiteral("diy"), WidgetCategory::ElectronicsDiy},
            {QStringLiteral("embedded"), WidgetCategory::EmbeddedSystems}};
        meta.category = categories.value(definition.value(QStringLiteral("group")).toString(),
                                         WidgetCategory::IndustrialInstrument);
        meta.icon = QStringLiteral("speed_high");
        meta.defaultWidth = 160;
        meta.defaultHeight = 140;
        const auto size = definition.value(QStringLiteral("size")).toArray();
        if (size.size() == 2) {
            meta.defaultWidth = size.at(0).toInt(160);
            meta.defaultHeight = size.at(1).toInt(140);
        }
        if (timing) {
            meta.defaultWidth = 480;
            meta.defaultHeight = 300;
        }
        meta.minWidth = 80;
        meta.minHeight = 72;
        const auto stringProp = [](const QString &name, const QString &label, const QJsonValue &value) {
            PropDef prop;
            prop.name = name;
            prop.label = label;
            prop.defaultValue = value;
            return prop;
        };
        const auto choice = [&](const QString &name, const QString &label, const QStringList &options) {
            auto prop = stringProp(name, label, options.first());
            prop.type = PropType::Enum;
            prop.options = options;
            return prop;
        };
        const auto number = [&](const QString &name, const QString &label, double value) {
            auto prop = stringProp(name, label, value);
            prop.type = PropType::Float;
            prop.minimum = -1e12;
            prop.maximum = 1e12;
            return prop;
        };
        meta.properties = {stringProp(QStringLiteral("title"), QString::fromUtf8(QT_TR_NOOP("标题")), meta.displayName)};
        if (capabilities.numericReadout) {
            meta.properties.append(stringProp(QStringLiteral("unit"), QString::fromUtf8(QT_TR_NOOP("单位")),
                                              definition.value(QStringLiteral("unit")).toString()));
        }
        if (!capabilities.structuredInput) {
            auto sample = number(QStringLiteral("value"), QString::fromUtf8(QT_TR_NOOP("设计示意值")),
                                 capabilities.sampleDefault);
            sample.bindable = true;
            if (capabilities.sampleMaximum >= 0) {
                sample.type = PropType::Int;
                sample.minimum = 0;
                sample.maximum = capabilities.sampleMaximum;
            } else if (meta.typeName == QLatin1String("RotaryEncoder")) {
                sample.minimum = -9007199254740991.0;
                sample.maximum = 9007199254740991.0;
            }
            meta.properties.append(sample);
        }
        if (capabilities.scale) {
            meta.properties += {
                number(QStringLiteral("minimum"), QString::fromUtf8(QT_TR_NOOP("最小值")),
                       definition.value(QStringLiteral("minimum")).toDouble()),
                number(QStringLiteral("maximum"), QString::fromUtf8(QT_TR_NOOP("最大值")),
                       definition.value(QStringLiteral("maximum")).toDouble(100))};
        }
        if (capabilities.numericReadout) {
            auto decimals = stringProp(QStringLiteral("decimals"), QString::fromUtf8(QT_TR_NOOP("小数位数")),
                                       definition.value(QStringLiteral("decimals")).toInt(1));
            decimals.type = PropType::Int;
            decimals.minimum = 0;
            decimals.maximum = 6;
            meta.properties.append(decimals);
        }
        const auto limits = definition.value(QStringLiteral("limits")).toArray();
        if (limits.size() == 2) {
            auto enabled = stringProp(QStringLiteral("showLimits"), QString::fromUtf8(QT_TR_NOOP("显示限值")),
                                      definition.value(QStringLiteral("limitsOn")).toBool());
            enabled.type = PropType::Bool;
            meta.properties +=
                {enabled,
                 number(QStringLiteral("lowLimit"), QString::fromUtf8(QT_TR_NOOP("低限")), limits.at(0).toDouble()),
                 number(QStringLiteral("highLimit"), QString::fromUtf8(QT_TR_NOOP("高限")), limits.at(1).toDouble())};
        }
        if (meta.typeName == QLatin1String("FlowPipe")) {
            auto width = number(QStringLiteral("pipeWidth"), QString::fromUtf8(QT_TR_NOOP("管径")), 16);
            width.minimum = 8;
            width.maximum = 26;
            meta.properties +=
                {choice(QStringLiteral("pipeShape"), QString::fromUtf8(QT_TR_NOOP("管道形状")),
                        {QString::fromUtf8(QT_TR_NOOP("弯管")), QString::fromUtf8(QT_TR_NOOP("直管"))}),
                 choice(QStringLiteral("pipeDirection"), QString::fromUtf8(QT_TR_NOOP("管道布局")),
                        {QString::fromUtf8(QT_TR_NOOP("正向布局")), QString::fromUtf8(QT_TR_NOOP("镜像布局"))}),
                 width};
        }
        if (meta.typeName == QLatin1String("Motor")) {
            meta.properties +=
                {choice(QStringLiteral("feedbackMode"), QString::fromUtf8(QT_TR_NOOP("反馈模式")),
                        {QString::fromUtf8(QT_TR_NOOP("转速")), QString::fromUtf8(QT_TR_NOOP("运行位"))})};
        }
        if (meta.typeName == QLatin1String("Cylinder")) {
            meta.properties +=
                {choice(QStringLiteral("feedbackMode"), QString::fromUtf8(QT_TR_NOOP("反馈模式")),
                        {QString::fromUtf8(QT_TR_NOOP("行程")), QString::fromUtf8(QT_TR_NOOP("端位状态"))})};
        }

        if (meta.typeName == QLatin1String("StatusWordPanel") || timing)
            meta.properties.append(stringProp(QStringLiteral("bitProfile"), QString::fromUtf8(QT_TR_NOOP("位配置")),
                                              QStringLiteral("default")));
        else if (meta.typeName == QLatin1String("RotaryEncoder")) {
            auto pulses = number(QStringLiteral("pulsesPerTurn"), QString::fromUtf8(QT_TR_NOOP("每圈脉冲")), 1024);
            pulses.minimum = 1;
            pulses.maximum = 9007199254740991.0;
            meta.properties.append(pulses);
        }
        const auto toggle = [&](const QString &name, const QString &label, bool value) {
            auto prop = stringProp(name, label, value);
            prop.type = PropType::Bool;
            return prop;
        };
        const auto offset = [&](const QString &name, const QString &label, int value) {
            auto prop = number(name, label, value);
            prop.type = PropType::Int;
            prop.minimum = -2000;
            prop.maximum = 2000;
            return prop;
        };
        meta.properties +=
            {toggle(QStringLiteral("showTitle"), QString::fromUtf8(QT_TR_NOOP("显示标题")), true),
             toggle(QStringLiteral("showValue"), QString::fromUtf8(QT_TR_NOOP("显示读值")), true)};
        if (!timing) {
            meta.properties +=
                {choice(QStringLiteral("textLayout"), QString::fromUtf8(QT_TR_NOOP("文字定位")),
                        {QString::fromUtf8(QT_TR_NOOP("图形周围")), QString::fromUtf8(QT_TR_NOOP("中心偏移"))})
                     .withHelp("industrial.text-layout"),
                 offset(QStringLiteral("titleOffsetX"), QString::fromUtf8(QT_TR_NOOP("标题水平偏移")), 0),
                 offset(QStringLiteral("titleOffsetY"), QString::fromUtf8(QT_TR_NOOP("标题垂直偏移")), -50),
                 offset(QStringLiteral("valueOffsetX"), QString::fromUtf8(QT_TR_NOOP("读值水平偏移")), 0),
                 offset(QStringLiteral("valueOffsetY"), QString::fromUtf8(QT_TR_NOOP("读值垂直偏移")), 0),
                 toggle(QStringLiteral("textPositionLocked"), QString::fromUtf8(QT_TR_NOOP("锁定文字位置")), false),
                 toggle(QStringLiteral("showToolTip"), QString::fromUtf8(QT_TR_NOOP("悬浮读值")), false)};
        }
        if (meta.typeName == QLatin1String("TractionBattery")) {
            const auto percent = [&](const QString &name, const QString &label, double value) {
                auto prop = number(name, label, value);
                prop.minimum = 0;
                prop.maximum = 100;
                return prop;
            };
            const auto color = [&](const QString &name, const QString &label, const QString &value) {
                auto prop = stringProp(name, label, value);
                prop.type = PropType::Color;
                return prop;
            };
            meta.properties +=
                {toggle(QStringLiteral("chargeColors"), QString::fromUtf8(QT_TR_NOOP("电量分段变色")), false)
                     .withHelp("display.threshold"),
                 percent(QStringLiteral("chargeLow"), QString::fromUtf8(QT_TR_NOOP("低电量上限(%)")), 20),
                 percent(QStringLiteral("chargeWarning"), QString::fromUtf8(QT_TR_NOOP("预警电量上限(%)")), 40),
                 color(QStringLiteral("chargeLowColor"), QString::fromUtf8(QT_TR_NOOP("低电量颜色")),
                            QStringLiteral("#e55252")),
                 color(QStringLiteral("chargeWarningColor"), QString::fromUtf8(QT_TR_NOOP("预警颜色")),
                            QStringLiteral("#e8b643")),
                 color(QStringLiteral("chargeNormalColor"), QString::fromUtf8(QT_TR_NOOP("正常颜色")),
                            QStringLiteral("#3dbd84"))};
        }
        result.append(meta);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const WidgetMeta &a, const WidgetMeta &b) { return a.category < b.category; });
    return result;
}
} // namespace fhmi
