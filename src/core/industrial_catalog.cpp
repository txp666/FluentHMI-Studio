#include "core/industrial_catalog.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <algorithm>

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
    for (const auto &item : industrialCatalog()) {
        const auto object = item.toObject();
        if (object.value(QStringLiteral("type")).toString() == type) {
            return object;
        }
    }
    return {};
}

bool isIndustrialCategory(WidgetCategory category)
{
    return category == WidgetCategory::IndustrialEquipment || category == WidgetCategory::IndustrialInstrument ||
           category == WidgetCategory::AutomotiveElectronics || category == WidgetCategory::ElectronicsDiy ||
           category == WidgetCategory::EmbeddedSystems;
}

QList<WidgetMeta> industrialWidgets()
{
    QList<WidgetMeta> result;
    for (const auto &item : industrialCatalog()) {
        const auto definition = item.toObject();
        WidgetMeta meta;
        meta.typeName = definition.value(QStringLiteral("type")).toString();
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
        meta.minWidth = 80;
        meta.minHeight = 72;
        const auto stringProp = [](const QString &name, const QString &label, const QJsonValue &value) {
            PropDef prop;
            prop.name = name;
            prop.label = label;
            prop.defaultValue = value;
            return prop;
        };
        meta.properties = {stringProp(QStringLiteral("title"), QString::fromUtf8(QT_TR_NOOP("标题")), meta.displayName),
                           stringProp(QStringLiteral("unit"), QString::fromUtf8(QT_TR_NOOP("单位")),
                                      definition.value(QStringLiteral("unit")).toString())};
        for (const auto &name : {QStringLiteral("value"), QStringLiteral("minimum"), QStringLiteral("maximum")}) {
            PropDef prop;
            prop.name = name;
            prop.label = name == QLatin1String("value")     ? QString::fromUtf8(QT_TR_NOOP("当前值"))
                         : name == QLatin1String("minimum") ? QString::fromUtf8(QT_TR_NOOP("最小值"))
                                                            : QString::fromUtf8(QT_TR_NOOP("最大值"));
            prop.type = PropType::Float;
            prop.defaultValue = definition.value(name).toDouble(name == QLatin1String("maximum") ? 100 : 0);
            prop.bindable = name == QLatin1String("value");
            prop.minimum = -1e12;
            prop.maximum = 1e12;
            meta.properties.append(prop);
        }
        PropDef decimals = stringProp(QStringLiteral("decimals"), QString::fromUtf8(QT_TR_NOOP("小数位数")),
                                      definition.value(QStringLiteral("decimals")).toInt(1));
        decimals.type = PropType::Int;
        decimals.minimum = 0;
        decimals.maximum = 6;
        meta.properties.append(decimals);
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

        if (meta.typeName == QLatin1String("StatusWordPanel") || meta.typeName == QLatin1String("DigitalTimingChart")) {
            meta.properties = {meta.properties.first(),
                               number(QStringLiteral("value"), QString::fromUtf8(QT_TR_NOOP("当前值")),
                                      definition.value(QStringLiteral("value")).toDouble()),
                               stringProp(QStringLiteral("bitProfile"), QString::fromUtf8(QT_TR_NOOP("位配置")),
                                          QStringLiteral("default"))};
            meta.properties[1].type = PropType::Int;
            meta.properties[1].minimum = 0;
            meta.properties[1].maximum = 65535;
            meta.properties[1].bindable = true;
        } else if (meta.typeName == QLatin1String("RotaryEncoder")) {
            meta.properties = {
                meta.properties.first(),
                number(QStringLiteral("value"), QString::fromUtf8(QT_TR_NOOP("当前值")),
                       definition.value(QStringLiteral("value")).toDouble()),
                number(QStringLiteral("pulsesPerTurn"), QString::fromUtf8(QT_TR_NOOP("每圈脉冲")), 1024)};
            meta.properties[1].minimum = -9007199254740991.0;
            meta.properties[1].maximum = 9007199254740991.0;
            meta.properties[1].bindable = true;
            meta.properties[2].type = PropType::Float;
            meta.properties[2].minimum = 1;
            meta.properties[2].maximum = 9007199254740991.0;
        } else if (meta.typeName == QLatin1String("BmsCellMatrix") || meta.typeName == QLatin1String("DcPowerSupply")) {
            meta.properties = {meta.properties.first()};
        }
        result.append(meta);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const WidgetMeta &a, const WidgetMeta &b) { return a.category < b.category; });
    return result;
}
} // namespace fhmi
