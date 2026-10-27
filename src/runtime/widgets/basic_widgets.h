#pragma once

#include "runtime/hmi_widget.h"

namespace FluentQt {
class BodyLabel;
class ComboBox;
class LineEdit;
class PushButton;
class Slider;
class SpinBox;
class SwitchButton;
} // namespace FluentQt

namespace fhmi {

class PushButtonWidget : public HmiWidget
{
  public:
    PushButtonWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::PushButton *m_button = nullptr;
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

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::LineEdit *m_edit = nullptr;
};

class ComboBoxWidget : public HmiWidget
{
  public:
    ComboBoxWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::ComboBox *m_combo = nullptr;
};

class ToggleSwitchWidget : public HmiWidget
{
  public:
    ToggleSwitchWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::BodyLabel *m_label = nullptr;
    FluentQt::SwitchButton *m_switch = nullptr;
};

class SliderWidget : public HmiWidget
{
  public:
    SliderWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::Slider *m_slider = nullptr;
    bool m_quiet = false;
};

class SpinBoxWidget : public HmiWidget
{
  public:
    SpinBoxWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void onAttached() override;

  private:
    FluentQt::SpinBox *m_spin = nullptr;
};

} // namespace fhmi
