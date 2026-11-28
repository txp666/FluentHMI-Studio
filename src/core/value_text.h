#pragma once
#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVariant>

namespace fhmi {
struct ValueText
{
    QString text;
    QString color;
};
// One numeric value=text|#RRGGBB per line; colour is optional.
QHash<QString, ValueText> parseValueText(const QString &text, QString *error = nullptr);
QString valueTextKey(const QVariant &value);
QString enumMapping(const QJsonObject &properties, const QJsonObject &tables);
bool usesMultiStateLed(const QJsonObject &properties);
// Keys use the same numeric identity as enum entries (e.g. 2 and 2.0).
QHash<QString, QString> parseLedEffects(const QJsonValue &effects, QString *error = nullptr);
} // namespace fhmi
