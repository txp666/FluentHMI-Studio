#include "core/widget_reference.h"

#include "core/industrial_catalog.h"
#include "core/widget_meta.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSet>
#include <QtCore/QStringList>

// The widget reference is documentation, in Chinese like the rest of docs/: nothing here is translated.
namespace fhmi {

namespace {

QString typeText(PropType type)
{
    switch (type) {
    case PropType::String:
        return QStringLiteral("文本"); // i18n: ignore
    case PropType::Int:
        return QStringLiteral("整数"); // i18n: ignore
    case PropType::Float:
        return QStringLiteral("小数"); // i18n: ignore
    case PropType::Bool:
        return QStringLiteral("true/false");
    case PropType::Color:
        return QStringLiteral("颜色"); // i18n: ignore
    case PropType::Enum:
        return QStringLiteral("选项"); // i18n: ignore
    case PropType::Text:
        return QStringLiteral("多行文本"); // i18n: ignore
    case PropType::Command:
        return QStringLiteral("命令"); // i18n: ignore
    case PropType::Object:
        return QStringLiteral("对象"); // i18n: ignore
    }
    return QString();
}

// A string as a .uidp spells it: JSON-escaped, so the newline option "\r\n" reads "\\r\\n".
QString jsonString(const QString &text)
{
    const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return QStringLiteral("`%1`").arg(QString::fromUtf8(array.mid(1, array.size() - 2)));
}

QString valueText(const QJsonValue &value)
{
    if (value.isString()) {
        return jsonString(value.toString());
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("`true`") : QStringLiteral("`false`");
    }
    if (value.isDouble()) {
        return QStringLiteral("`%1`").arg(value.toDouble());
    }
    if (value.isObject()) {
        return QStringLiteral("`%1`").arg(
            QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)));
    }
    return QStringLiteral("`\"\"`");
}

QString propertyReference(const PropDef &prop)
{
    QString out;
    out += QStringLiteral("- `%1`（%2，%3）默认 %4") // i18n: ignore
               .arg(prop.name, prop.label, typeText(prop.type), valueText(prop.defaultValue));
    if (prop.minimum && prop.maximum) {
        out += QStringLiteral("，范围 %1–%2") // i18n: ignore; whole numbers in full, as in the Python reference
                   .arg(QString::number(*prop.minimum, 'g', 15), QString::number(*prop.maximum, 'g', 15));
    }
    if (!prop.options.isEmpty()) {
        QStringList options;
        for (const QString &option : prop.options) {
            options << jsonString(option);
        }
        out += QStringLiteral("，可选 ") + options.join(QLatin1Char(' ')); // i18n: ignore
    }
    if (!prop.description.isEmpty()) {
        out += QStringLiteral("，") + prop.description; // i18n: ignore
    }
    out += QLatin1Char('\n');
    return out;
}

// Compare the complete definition, so a later registry change stays local to its widget.
bool sameProperty(const PropDef &a, const PropDef &b)
{
    return a.name == b.name && a.label == b.label && a.type == b.type && a.defaultValue == b.defaultValue &&
           a.options == b.options && a.minimum == b.minimum && a.maximum == b.maximum && a.bindable == b.bindable &&
           a.description == b.description;
}

struct SharedProperties
{
    QString anchor;
    QString title;
    QStringList types;
    QList<PropDef> properties;
};

QList<SharedProperties> sharedProperties()
{
    QList<SharedProperties> groups{
        {QStringLiteral("command-properties"),
         QStringLiteral("命令控件共有属性"), // i18n: ignore
         {QStringLiteral("PushButton"), QStringLiteral("LineEdit"), QStringLiteral("ComboBox"),
          QStringLiteral("ToggleSwitch"), QStringLiteral("Slider"), QStringLiteral("SpinBox")},
         {}},
        {QStringLiteral("link-properties"),
         QStringLiteral("Modbus 轮询链路卡片共有属性"), // i18n: ignore
         {QStringLiteral("SerialConfigCard"), QStringLiteral("TcpConfigCard"), QStringLiteral("UdpConfigCard"),
          QStringLiteral("MqttConfigCard")},
         {}}};
    const auto &registry = WidgetRegistry::instance();
    for (auto &group : groups) {
        const auto *first = registry.find(group.types.first());
        if (!first) {
            continue;
        }
        for (const auto &prop : first->properties) {
            bool shared = true;
            for (const auto &type : group.types) {
                const auto *meta = registry.find(type);
                const auto *other = meta ? meta->prop(prop.name) : nullptr;
                if (!other || !sameProperty(prop, *other)) {
                    shared = false;
                    break;
                }
            }
            if (shared) {
                group.properties.append(prop);
            }
        }
    }
    return groups;
}

} // namespace

QString widgetIndex()
{
    QString out;
    QStringList line;
    WidgetCategory category{};
    for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
        if (isIndustrialCategory(meta.category)) {
            continue;
        }
        if (!line.isEmpty() && meta.category != category) {
            out += QStringLiteral("- %1：%2\n") // i18n: ignore
                       .arg(categoryTitle(category),
                            line.join(QStringLiteral("、"))); // i18n: ignore
            line.clear();
        }
        category = meta.category;
        line << QStringLiteral("`%1` %2（%3×%4）") // i18n: ignore
                    .arg(meta.typeName, meta.displayName)
                    .arg(meta.defaultWidth)
                    .arg(meta.defaultHeight);
    }
    if (!line.isEmpty()) {
        out +=
            QStringLiteral("- %1：%2\n").arg(categoryTitle(category), line.join(QStringLiteral("、"))); // i18n: ignore
    }
    return QLatin1Char('\n') + out +
           QStringLiteral("- [工业设备与工业仪表的类型和默认值](uidp/industrial.md)\n"           // i18n: ignore
                          "- [汽车电子、电子 DIY 与嵌入式的类型和状态](uidp/electronics.md)\n"); // i18n: ignore
}

QString widgetReference()
{
    QString out;
    const auto groups = sharedProperties();
    for (const auto &group : groups) {
        out += QStringLiteral("\n<a id=\"%1\"></a>\n\n### %2\n\n").arg(group.anchor, group.title);
        QStringList names;
        for (const auto &type : group.types) {
            names << QStringLiteral("`%1`").arg(type);
        }
        out += QStringLiteral("适用于 %1。\n\n").arg(names.join(QStringLiteral("、"))); // i18n: ignore
        for (const auto &prop : group.properties) {
            out += propertyReference(prop);
        }
    }
    bool first = true;
    WidgetCategory category{};
    for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
        if (isIndustrialCategory(meta.category)) {
            continue;
        }
        if (first || meta.category != category) {
            out += QStringLiteral("\n### %1\n").arg(categoryTitle(meta.category));
            category = meta.category;
            first = false;
        }
        out += QStringLiteral("\n#### `%1` %2\n\n").arg(meta.typeName, meta.displayName);
        out += QStringLiteral("默认尺寸 %1×%2，最小 %3×%4") // i18n: ignore
                   .arg(meta.defaultWidth)
                   .arg(meta.defaultHeight)
                   .arg(meta.minWidth)
                   .arg(meta.minHeight);
        if (meta.container) {
            out += QStringLiteral("；**容器**，子控件放在 `children` 里"); // i18n: ignore
        }
        if (!meta.bindProperty().isEmpty()) {
            out += QStringLiteral("；绑定数据时写入 `%1`").arg(meta.bindProperty()); // i18n: ignore
        }
        out += QStringLiteral("\n\n");
        QSet<QString> sharedNames;
        for (const auto &group : groups) {
            if (group.types.contains(meta.typeName)) {
                out += QStringLiteral("另见[%1](#%2)。\n\n").arg(group.title, group.anchor); // i18n: ignore
                for (const auto &common : group.properties) {
                    sharedNames.insert(common.name);
                }
            }
        }
        for (const PropDef &prop : meta.properties) {
            if (!sharedNames.contains(prop.name)) {
                out += propertyReference(prop);
            }
        }
    }
    return out +
           QStringLiteral("\n### "
                          "工业设备与仪表\n\n共用属性、各类型默认值和设备特有选项见[工业控件参考](uidp/" // i18n: ignore
                          "industrial.md)。\n" // i18n: ignore
                          "\n### 汽车电子、电子 DIY 与嵌入式\n\n" // i18n: ignore
                          "只读控件的类型、默认值和状态见[电子控件参考](uidp/electronics.md)。\n"); // i18n: ignore
}

QString withGenerated(const QString &document, const QString &generated)
{
    const qsizetype at = document.indexOf(GeneratedMarker);
    if (at < 0) {
        return QString();
    }
    return document.left(at + GeneratedMarker.size()) + QLatin1Char('\n') + generated;
}

} // namespace fhmi
