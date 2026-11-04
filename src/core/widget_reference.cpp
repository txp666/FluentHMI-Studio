#include "core/widget_reference.h"

#include "core/widget_meta.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
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

} // namespace

QString widgetIndex()
{
    QString out;
    QStringList line;
    WidgetCategory category{};
    for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
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
        for (const PropDef &prop : meta.properties) {
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
