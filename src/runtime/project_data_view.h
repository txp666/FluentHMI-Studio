#pragma once

class QWidget;
class QJsonObject;

namespace fhmi {
class ProjectData;
void openProjectDataMonitor(const QJsonObject &logic, ProjectData *data, QWidget *parent);
} // namespace fhmi
