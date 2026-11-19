#pragma once
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>

namespace fhmi {
class Project;
// Validates names, binding completeness, formula references and cycles once, at the project boundary.
QString projectLogicProblem(const QJsonObject &logic);
QStringList dataPointOrder(const QJsonObject &logic);
QStringList dataPointNames(const QJsonObject &logic, bool parametersOnly = false);
QJsonObject findDataPoint(const QJsonObject &logic, const QString &name);
QString dataPointSourceProblem(const QJsonObject &binding, const QJsonArray &widgets, const QString &projectParser);
QString dataPointSourceText(const QJsonObject &binding);
QStringList dataPointReferences(const Project &project, const QString &name);
bool isProjectParameter(const QJsonObject &point);
QString dataPointCommandText(const QVariant &value);
QString pointInputId(const QString &name);
QString pointNameForInput(const QString &id);
bool isLegacyPointInput(const QString &id);
QString widgetInputId(const QString &id);
QString widgetIdForInput(const QString &id);
// The editor validates the new name before applying this project-wide rename.
void renameDataPoint(Project *project, const QString &previous, const QString &current);
} // namespace fhmi
