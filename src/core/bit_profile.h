#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QString>
namespace fhmi {
int bitProfileWidth(const QJsonObject &profiles, const QString &name);
QJsonObject effectiveBitProfile(const QJsonObject &profiles, const QString &name);
QString bitProfilesProblem(const QJsonObject &profiles);
} // namespace fhmi
