#pragma once

#include <QtCore/QStringView>
#include <optional>

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
    DataRouting,
    DataBinding,
    TransformExpression,
    CommandTemplates,
    ModbusAddressing,
    S7Addressing,
    ReceiveFraming,
    IndustrialTextLayout,
    EnumDisplay,
    ValueThreshold,
    ProjectLogic,
    CanRecovery,
    PageNavigation,
    WidgetAppearance,
    SelectionStyles,
    NumberInput,
    AlarmFilters,
};

// Opens a read-only help card with selectable explanations and independently copyable examples.
// Shared by the designer and runtime; it never edits a project or calls a communication link.
QToolButton *createFieldHelpButton(FieldHelpTopic topic, QWidget *parent);
std::optional<FieldHelpTopic> fieldHelpTopic(QStringView key);

} // namespace fhmi
