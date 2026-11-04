#pragma once

#include "runtime/hmi_widget.h"

namespace FluentQt {
class BodyLabel;
class CaptionLabel;
class ImageLabel;
class ProgressBar;
class ProgressRing;
} // namespace FluentQt

namespace fhmi {

bool isDarkTheme();
QColor accentColor();

class LedWidget : public HmiWidget
{
  public:
    LedWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    FluentQt::BodyLabel *m_label = nullptr;
    bool m_on = true;
};

class NumericDisplayWidget : public HmiWidget
{
  public:
    NumericDisplayWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    FluentQt::CaptionLabel *m_title = nullptr;
    FluentQt::BodyLabel *m_value = nullptr;
    FluentQt::CaptionLabel *m_unit = nullptr;
};

class ProgressBarWidget : public HmiWidget
{
  public:
    ProgressBarWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;

  private:
    FluentQt::ProgressBar *m_bar = nullptr;
};

class ProgressRingWidget : public HmiWidget
{
  public:
    ProgressRingWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    FluentQt::ProgressRing *m_ring = nullptr;
    FluentQt::BodyLabel *m_label = nullptr;
};

class ImageWidget : public HmiWidget
{
  public:
    ImageWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void refresh();

    FluentQt::ImageLabel *m_image = nullptr;
    FluentQt::CaptionLabel *m_placeholder = nullptr;
};

class GaugeWidget : public HmiWidget
{
  public:
    GaugeWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    double m_value = 0;
};

} // namespace fhmi
