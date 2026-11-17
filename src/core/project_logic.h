#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

namespace fhmi {
class Project;
// Validates names, binding completeness, formula references and cycles once, at the project boundary.
QString projectLogicProblem(const QJsonObject &logic);
QStringList dataPointOrder(const QJsonObject &logic);
// The editor validates the new name before applying this project-wide rename.
void renameDataPoint(Project *project, const QString &previous, const QString &current);
} // namespace fhmi
