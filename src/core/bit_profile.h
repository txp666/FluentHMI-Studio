#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QString>
namespace fhmi {
QJsonObject effectiveBitProfile(const QJsonObject &profiles, const QString &name);
QString bitProfilesProblem(const QJsonObject &profiles);
} // namespace fhmi
