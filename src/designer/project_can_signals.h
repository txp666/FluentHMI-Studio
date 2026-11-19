#pragma once

#include "designer/reference_impact_dialog.h"

namespace fhmi {

class Project;
struct CanSignalEdit;

// Stage the card definition and its unambiguous references in the same project draft.
QList<ReferenceRow> stageCanSignalEdit(Project &draft, const QString &cardId, const CanSignalEdit &edit);

} // namespace fhmi
