#pragma once
#include <QtCore/QHash>
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
} // namespace fhmi
