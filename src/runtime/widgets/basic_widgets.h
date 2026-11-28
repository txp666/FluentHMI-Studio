#pragma once

#include "core/command_encoder.h"
#include "runtime/hmi_widget.h"

class QAbstractSpinBox;
class QButtonGroup;

namespace FluentQt {
class BodyLabel;
class CaptionLabel;
class CheckBox;
class ComboBox;
class DoubleSpinBox;
class LineEdit;
class PushButton;
class SegmentedWidget;
class SingleDirectionScrollArea;
class Slider;
class SpinBox;
class SwitchButton;
} // namespace FluentQt

namespace fhmi {

class PushButtonWidget : public HmiWidget
{
  public:
    PushButtonWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void periodicSendChanged(bool running) override;

  private:
    void setButtonStyle(const QString &style);
    void sendCommand();
    CommandResult configuredCommand(const QVariant &value = {}) const override;

    FluentQt::PushButton *m_button = nullptr;
    QString m_buttonStyle;
    QString m_caption;
};

class LabelWidget : public HmiWidget
{
  public:
    LabelWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;

  private:
    FluentQt::BodyLabel *m_label = nullptr;
};

class LineEditWidget : public HmiWidget
{
  public:
    LineEditWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;
    std::optional<QVariant> inputValue() const override;

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::BodyLabel *m_title = nullptr;
    FluentQt::LineEdit *m_edit = nullptr;
};

class ComboBoxWidget : public HmiWidget
{
  public:
    ComboBoxWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void bindingStatusChanged(BindingStatus status) override;

  private:
    void rebuildSelection(const QString &style, const QList<CommandEncoder::SelectionOption> &options);
    void setCurrentIndex(int index);
    void selectIndex(int index);
    void ensureSelectionVisible();

    QWidget *m_selection = nullptr;
    FluentQt::SingleDirectionScrollArea *m_selectionScroll = nullptr;
    FluentQt::ComboBox *m_combo = nullptr;
    FluentQt::SegmentedWidget *m_segmented = nullptr;
    QButtonGroup *m_radioGroup = nullptr;
    QString m_selectionStyle;
    QList<CommandEncoder::SelectionOption> m_options;
    int m_currentIndex = -1;
    int m_configuredIndex = 0;
    bool m_applied = false;
};

class ToggleSwitchWidget : public HmiWidget
{
  public:
    ToggleSwitchWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;

  private:
    void setSwitchStyle(const QString &style);
    void setChecked(bool checked);
    void checkedEdited(bool checked);
    CommandResult configuredCommand(const QVariant &value) const override;

    QWidget *m_control = nullptr;
    FluentQt::BodyLabel *m_label = nullptr;
    FluentQt::SwitchButton *m_switch = nullptr;
    FluentQt::CheckBox *m_checkBox = nullptr;
    QString m_switchStyle;
    bool m_checked = false;
    bool m_configuredChecked = false;
    bool m_applied = false;
};

class SliderWidget : public HmiWidget
{
  public:
    SliderWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void setValue(int value);
    void sendValue(int value);

    FluentQt::Slider *m_slider = nullptr;
    FluentQt::CaptionLabel *m_minLabel = nullptr; // showRange: the minimum at the left end
    FluentQt::CaptionLabel *m_maxLabel = nullptr; // showRange: the maximum at the right end
    bool m_quiet = false;
    bool m_held = false; // the mouse is down on the slider: a drag sends once, on release
    int m_pressValue = 0;
    int m_value = 0;
};

class SpinBoxWidget : public HmiWidget
{
  public:
    SpinBoxWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;

  private:
    void setNumberMode(bool decimal);
    void setValue(double value);
    void valueEdited(double value);

    QAbstractSpinBox *m_editor = nullptr;
    FluentQt::SpinBox *m_spin = nullptr;
    FluentQt::DoubleSpinBox *m_doubleSpin = nullptr;
    double m_value = 0;
    double m_configuredValue = 0;
    bool m_applied = false;
    bool m_editingText = false;
};

} // namespace fhmi
