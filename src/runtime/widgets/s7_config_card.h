#pragma once
#include "runtime/hmi_widget.h"
#include <QtCore/QElapsedTimer>
class QGridLayout;
namespace FluentQt {
class LineEdit;
class SpinBox;
class ComboBox;
class PrimaryPushButton;
class CaptionLabel;
class StrongBodyLabel;
} // namespace FluentQt
namespace fhmi {
class S7ConfigCardWidget final : public HmiWidget
{
  public:
    S7ConfigCardWidget(const WidgetMeta &meta, QWidget *parent);
    ~S7ConfigCardWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void toggle();
    void refresh();
    void relayout();
    void showFailure(const QString &reason);
    QString id() const { return property("fhmiWidgetId").toString(); }
    FluentQt::LineEdit *m_host;
    FluentQt::SpinBox *m_port, *m_rack, *m_slot;
    FluentQt::ComboBox *m_type;
    FluentQt::PrimaryPushButton *m_button;
    FluentQt::CaptionLabel *m_status;
    FluentQt::StrongBodyLabel *m_title;
    QGridLayout *m_layout;
    QList<FluentQt::CaptionLabel *> m_labels;
    bool m_open = false;
    QString m_lastError;
    QElapsedTimer m_lastErrorAt;
};
} // namespace fhmi
