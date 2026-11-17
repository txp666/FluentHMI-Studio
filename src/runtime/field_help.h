#pragma once

class QToolButton;
class QWidget;

namespace fhmi {

enum class FieldHelpTopic
{
    CanInitialState,
    CanSignalCommand,
    CanSignalDefinitions,
    CanTiming,
    CanReceiveFilter,
    CanMonitorModes,
};

// Opens a read-only help card with selectable explanations and independently copyable examples.
// Shared by the designer and runtime; it never edits a project or calls a communication link.
QToolButton *createFieldHelpButton(FieldHelpTopic topic, QWidget *parent);

} // namespace fhmi
