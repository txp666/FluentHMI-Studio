#include "core/widget_reference.h"

#include "core/widget_meta.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QStringList>

namespace fhmi {

namespace {

QString typeText(PropType type)
{
    switch (type) {
    case PropType::String:
        return QStringLiteral("文本");
    case PropType::Int:
        return QStringLiteral("整数");
    case PropType::Float:
        return QStringLiteral("小数");
    case PropType::Bool:
        return QStringLiteral("true/false");
    case PropType::Color:
        return QStringLiteral("颜色");
    case PropType::Enum:
        return QStringLiteral("选项");
    case PropType::Text:
        return QStringLiteral("多行文本");
    case PropType::Command:
        return QStringLiteral("命令");
    case PropType::Object:
        return QStringLiteral("对象");
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

} // namespace

QString widgetIndex()
{
    QString out;
    QStringList line;
    WidgetCategory category{};
    for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
        if (!line.isEmpty() && meta.category != category) {
            out += QStringLiteral("- %1：%2\n").arg(categoryTitle(category), line.join(QStringLiteral("、")));
            line.clear();
        }
        category = meta.category;
        line << QStringLiteral("`%1` %2（%3×%4）")
                    .arg(meta.typeName, meta.displayName)
                    .arg(meta.defaultWidth)
                    .arg(meta.defaultHeight);
    }
    if (!line.isEmpty()) {
        out += QStringLiteral("- %1：%2\n").arg(categoryTitle(category), line.join(QStringLiteral("、")));
    }
    return QLatin1Char('\n') + out;
}

QString widgetReference()
{
    QString out;
    bool first = true;
    WidgetCategory category{};
    for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
        if (first || meta.category != category) {
            out += QStringLiteral("\n### %1\n").arg(categoryTitle(meta.category));
            category = meta.category;
            first = false;
        }
        out += QStringLiteral("\n#### `%1` %2\n\n").arg(meta.typeName, meta.displayName);
        out += QStringLiteral("默认尺寸 %1×%2，最小 %3×%4")
                   .arg(meta.defaultWidth)
                   .arg(meta.defaultHeight)
                   .arg(meta.minWidth)
                   .arg(meta.minHeight);
        if (meta.container) {
            out += QStringLiteral("；**容器**，子控件放在 `children` 里");
        }
        if (!meta.bindProperty().isEmpty()) {
            out += QStringLiteral("；绑定数据时写入 `%1`").arg(meta.bindProperty());
        }
        out += QStringLiteral("\n\n");
        for (const PropDef &prop : meta.properties) {
            out += QStringLiteral("- `%1`（%2，%3）默认 %4")
                       .arg(prop.name, prop.label, typeText(prop.type), valueText(prop.defaultValue));
            if (prop.minimum && prop.maximum) {
                out += QStringLiteral("，范围 %1–%2")  // whole numbers in full, as in the Python reference
                           .arg(QString::number(*prop.minimum, 'g', 15), QString::number(*prop.maximum, 'g', 15));
            }
            if (!prop.options.isEmpty()) {
                QStringList options;
                for (const QString &option : prop.options) {
                    options << jsonString(option);
                }
                out += QStringLiteral("，可选 ") + options.join(QLatin1Char(' '));
            }
            if (!prop.description.isEmpty()) {
                out += QStringLiteral("，") + prop.description;
            }
            out += QLatin1Char('\n');
        }
    }
    return out;
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
