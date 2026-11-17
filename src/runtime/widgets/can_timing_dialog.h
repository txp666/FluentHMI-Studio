#pragma once

#include "comm/can/zlg_timing.h"
#include "runtime/app_dialog.h"

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace fhmi {

// Built-in SJA1000 calculator. Selecting a result only fills the card; opening/restarting hardware is left
// to its normal Connect action. Other ZLG models retain their own calculator-string input.
class CanTimingDialog : public AppDialog
{
    Q_OBJECT

  public:
    explicit CanTimingDialog(const CanSettings &settings, QWidget *parent = nullptr);
    QString selectedTiming() const;

  private:
    void calculate();
    void updateSelection();
    QLineEdit *m_rate = nullptr;
    QDoubleSpinBox *m_sample = nullptr;
    QSpinBox *m_sjw = nullptr;
    QDoubleSpinBox *m_error = nullptr;
    QCheckBox *m_triple = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_copy = nullptr;
    QPushButton *m_apply = nullptr;
    QList<zlg::SjaTimingCandidate> m_candidates;
};

} // namespace fhmi
