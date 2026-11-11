#include "core/bit_profile.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
namespace fhmi {
namespace {
bool colorValid(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return pattern.match(value).hasMatch();
}
} // namespace
QJsonObject effectiveBitProfile(const QJsonObject &profiles, const QString &name)
{
    const auto source = profiles.value(name).toObject();
    const int width = source.value(QStringLiteral("width")).toInt(8) == 16 ? 16 : 8;
    const auto original = source.value(QStringLiteral("bits")).toArray();
    QJsonArray bits;
    for (int i = 0; i < width; ++i) {
        const auto bit = i < original.size() ? original.at(i).toObject() : QJsonObject();
        const auto color = bit.value(QStringLiteral("color")).toString();
        bits.append(QJsonObject{
            {QStringLiteral("name"), bit.value(QStringLiteral("name")).toString(QStringLiteral("B%1").arg(i))},
            {QStringLiteral("color"), colorValid(color) ? color : QStringLiteral("#228866")}});
    }
    return {{QStringLiteral("width"), width}, {QStringLiteral("bits"), bits}};
}
QString bitProfilesProblem(const QJsonObject &profiles)
{
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        const auto profile = it.value().toObject();
        const auto width = profile.value(QStringLiteral("width"));
        if (it.key().trimmed().isEmpty() || !it.value().isObject() ||
            (!width.isUndefined() && width != QJsonValue(8) && width != QJsonValue(16)))
            return QCoreApplication::translate("fhmi", "位配置 %1 必须是8或16位对象").arg(it.key());
        const auto values = profile.value(QStringLiteral("bits"));
        if (!values.isUndefined() && !values.isArray())
            return QCoreApplication::translate("fhmi", "位配置 %1 的 bits 必须是数组").arg(it.key());
        for (const auto &value : values.toArray()) {
            const auto bit = value.toObject();
            const auto name = bit.value(QStringLiteral("name")), color = bit.value(QStringLiteral("color"));
            if (!value.isObject() || (!name.isUndefined() && !name.isString()) ||
                (!color.isUndefined() && (!color.isString() || !colorValid(color.toString()))))
                return QCoreApplication::translate("fhmi", "位配置 %1 的位名应为文本、颜色应为 #RRGGBB").arg(it.key());
        }
    }
    return {};
}
} // namespace fhmi
