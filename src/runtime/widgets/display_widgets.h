#pragma once

#include "core/value_text.h"
#include "runtime/hmi_widget.h"

#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QTransform>

namespace FluentQt {
class BodyLabel;
class CaptionLabel;
class InfoBadge;
class ProgressBar;
class ProgressRing;
} // namespace FluentQt

namespace fhmi {

bool isDarkTheme();
QColor accentColor();
// Shared card surface; titles and child geometry stay with each widget.
void paintCardSurface(QWidget *widget, const QString &style);

class LedWidget : public HmiWidget
{
  public:
    LedWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void bindingStatusChanged(BindingStatus status) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

  private:
    qreal lampDiameter() const;
    void updateLayoutGeometry();
    void updateBlinkTimer();
    void showEnumValue(const QVariant &value);
    void activate();

    FluentQt::BodyLabel *m_label = nullptr;
    QTimer m_blinkTimer;
    QHash<QString, ValueText> m_entries;
    QHash<QString, QString> m_effects;
    QVariant m_reading;
    QString m_stateLabel;
    QColor m_stateColor;
    int m_halfPeriod = 0;
    bool m_multiState = false;
    bool m_blinkVisible = true;
    bool m_on = true;
    bool m_known = true;
    bool m_pressed = false;
};

class NumericDisplayWidget : public HmiWidget
{
  public:
    NumericDisplayWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void bindingStatusChanged(BindingStatus status) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void showValue(const QVariant &value);
    QVariant m_reading;
    QHash<QString, ValueText> m_enumEntries;
    FluentQt::CaptionLabel *m_title = nullptr;
    FluentQt::BodyLabel *m_value = nullptr;
    FluentQt::InfoBadge *m_badge = nullptr;
    FluentQt::CaptionLabel *m_unit = nullptr;
};

class ProgressBarWidget : public HmiWidget
{
  public:
    ProgressBarWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void bindingStatusChanged(BindingStatus) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    FluentQt::ProgressBar *m_bar = nullptr;
    QList<ColorRange> m_ranges;
    std::optional<double> m_reading;
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

// Where an ImageDisplay draws its picture: maps the picture's pixel coordinates into a widget box of `box` logical
// pixels. The picture is scaled by `scaleMode` (适应, 拉伸 or 原始大小), mirrored in its own frame, then turned
// `rotation` degrees clockwise about the box centre, and always fits inside the box as a whole.
QTransform imageTransform(const QSizeF &box, const QSize &image, int rotation, bool mirrorH, bool mirrorV,
                          const QString &scaleMode);

class ImageWidget : public HmiWidget
{
  public:
    ImageWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    FluentQt::CaptionLabel *m_placeholder = nullptr;
    QImage m_image; // the picture at its own size; painted through imageTransform, so it stays sharp at any scale
    QString m_scaleMode;
    int m_rotation = 0;
    bool m_mirrorH = false;
    bool m_mirrorV = false;
};

class GaugeWidget : public HmiWidget
{
  public:
    GaugeWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;

  protected:
    void apply() override;
    void bindingStatusChanged(BindingStatus status) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    double m_value = 0;
    QList<ColorRange> m_ranges;
    bool m_valid = true;
};

} // namespace fhmi
