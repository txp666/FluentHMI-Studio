#include "core/value_text.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <cmath>

namespace fhmi {
QString valueTextKey(const QVariant &value)
{
    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok && std::isfinite(number) ? QString::number(number, 'g', 17) : QString();
}
QList<EnumEntry> orderedEnumEntries(const QString &text, QString *error)
{
    QList<EnumEntry> entries;
    QSet<QString> keys;
    const auto lines = text.split(QLatin1Char('\n'));
    static const QRegularExpression color(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines[i].trimmed();
        if (line.isEmpty())
            continue;
        const int equal = line.indexOf(QLatin1Char('='));
        const QString code = line.left(equal).trimmed();
        const QString key = valueTextKey(code);
        const auto parts = line.mid(equal + 1).split(QLatin1Char('|'));
        const QString tint = parts.value(1).trimmed();
        if (equal < 1 || key.isEmpty() || keys.contains(key) || parts.size() > 2 || parts[0].trimmed().isEmpty() ||
            (!tint.isEmpty() && !color.match(tint).hasMatch())) {
            if (error)
                *error =
                    QCoreApplication::translate("fhmi", "枚举映射第 %1 行无效：使用 数值=文字|#RRGGBB，数值不能重复")
                        .arg(i + 1);
            return {};
        }
        keys.insert(key);
        entries.append({code, {parts[0].trimmed(), tint}});
    }
    return entries;
}

QHash<QString, ValueText> parseValueText(const QString &text, QString *error)
{
    QHash<QString, ValueText> result;
    for (const auto &entry : orderedEnumEntries(text, error))
        result.insert(valueTextKey(entry.code), entry.display);
    return result;
}

QString enumMapping(const QJsonObject &properties, const QJsonObject &tables)
{
    const auto name = properties.value(QStringLiteral("enumTable")).toString();
    return name.isEmpty() ? properties.value(QStringLiteral("textList")).toString() : tables.value(name).toString();
}

bool usesMultiStateLed(const QJsonObject &properties)
{
    return properties.value(QStringLiteral("stateMode")).toString() == QStringLiteral("多状态"); // i18n: value
}

QHash<QString, QString> parseLedEffects(const QJsonValue &effects, QString *error)
{
    QHash<QString, QString> result;
    if (effects.isUndefined())
        return result;
    const auto object = effects.toObject();
    bool valid = effects.isObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto key = valueTextKey(it.key());
        const auto effect = it.value().toString();
        if (key.isEmpty() || result.contains(key) ||
            (effect != u"off" && effect != u"steady" && effect != u"slow" && effect != u"fast")) {
            valid = false;
            break;
        }
        result.insert(key, effect);
    }
    if (!valid) {
        if (error)
            *error = QCoreApplication::translate("fhmi", "灯效需要唯一数值键，取值为 off、steady、slow 或 fast");
        return {};
    }
    return result;
}
} // namespace fhmi
