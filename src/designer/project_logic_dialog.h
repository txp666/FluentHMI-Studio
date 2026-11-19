#pragma once
#include "core/project.h"
class QWidget;
namespace fhmi {
// Edits a copy; accepting commits one undoable project change.
bool editProjectLogic(Project *project, QWidget *parent);
bool editDevicePoint(Project *project, QJsonObject *point, QWidget *parent, bool editName);
} // namespace fhmi
