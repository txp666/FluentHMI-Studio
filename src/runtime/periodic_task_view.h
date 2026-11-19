#pragma once

class QPushButton;
class QWidget;

namespace fhmi {
class PeriodicSender;

// The same entry is available in Preview and the standalone player.
QPushButton *createPeriodicTasksButton(PeriodicSender *sender, QWidget *parent);
} // namespace fhmi
