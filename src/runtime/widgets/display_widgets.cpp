#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ProgressBar.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fhmi {

bool isDarkTheme()
{
    return FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
}

QColor accentColor()
{
    return FluentQt::ThemeManager::instance()->accentColor();
}

namespace {

void repaintOnThemeChange(QWidget *widget)
{
    QObject::connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, widget,
                     [widget]() { widget->update(); });
    QObject::connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::accentColorChanged, widget,
                     [widget]() { widget->update(); });
}

QColor ledColor(const QString &name)
{
    if (name == QLatin1String("red")) {
        return QColor(0xE8, 0x48, 0x3F);
    }
    if (name == QLatin1String("yellow")) {
        return QColor(0xF7, 0xB9, 0x00);
    }
    if (name == QLatin1String("blue")) {
        return QColor(0x29, 0x8D, 0xF2);
    }
    if (name == QLatin1String("gray")) {
        return QColor(0x9E, 0x9E, 0x9E);
    }
    return QColor(0x3C, 0xC0, 0x6A);
}

void paintCard(QWidget *widget)
{
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

} // namespace

// ---------------------------------------------------------------------------
// LED
// ---------------------------------------------------------------------------

LedWidget::LedWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(34, 0, 4, 0);
    m_label = new FluentQt::BodyLabel(this);
    layout->addWidget(m_label);
    repaintOnThemeChange(this);
}

void LedWidget::apply()
{
    m_label->setText(text("label"));
    m_on = flag("on");
    update();
}

void LedWidget::applyBoundValue(const QVariant &value, const BindingEntry &binding)
{
    if (const auto on = bindingLed(value, binding.ledOnValue, binding.ledOffValue)) {
        m_on = *on;
        update();
    }
}

void LedWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal d = std::min<qreal>(18, height() - 4);
    // The glow reaches past the dot; keep all of it inside the widget.
    const qreal glowRadius = std::min<qreal>(d * 0.8, height() / 2.0);
    const QRectF dot(glowRadius + 1 - d / 2, (height() - d) / 2, d, d);
    const QColor color = ledColor(text("color"));
    if (m_on) {
        QRadialGradient glow(dot.center(), glowRadius);
        QColor halo = color;
        halo.setAlpha(90);
        glow.setColorAt(0.55, halo);
        glow.setColorAt(1, Qt::transparent);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(dot.center(), glowRadius, glowRadius);
        painter.setBrush(color);
    } else {
        painter.setBrush(isDarkTheme() ? QColor(70, 70, 70) : QColor(205, 205, 205));
    }
    painter.setPen(QPen(isDarkTheme() ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 40), 1));
    painter.drawEllipse(dot);
}

// ---------------------------------------------------------------------------
// NumericDisplay
// ---------------------------------------------------------------------------

NumericDisplayWidget::NumericDisplayWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    // Title, value and unit stacked and centred, as PyQt UI Designer lays them out.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(0);
    m_title = new FluentQt::CaptionLabel(this);
    m_value = new QLabel(this);
    m_unit = new FluentQt::CaptionLabel(this);
    for (QLabel *label : {static_cast<QLabel *>(m_title), m_value, static_cast<QLabel *>(m_unit)}) {
        label->setAlignment(Qt::AlignCenter);
    }
    layout->addStretch();
    layout->addWidget(m_title);
    layout->addWidget(m_value);
    layout->addWidget(m_unit);
    layout->addStretch();
    repaintOnThemeChange(this);
    const auto restyle = [this]() { apply(); };
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this, restyle);
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::accentColorChanged, this, restyle);
}

void NumericDisplayWidget::apply()
{
    m_title->setText(text("title"));
    m_unit->setText(text("unit"));
    m_unit->setVisible(!text("unit").isEmpty());
    m_value->setText(text("value"));
    QFont font = m_value->font();
    font.setPixelSize(std::max(8, integer("fontSize"))); // pixels, like the Python designer
    font.setBold(true);
    m_value->setFont(font);
    m_value->setStyleSheet(QStringLiteral("color: %1;").arg(accentColor().name()));
}

void NumericDisplayWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    m_value->setText(displayString(value));
}

void NumericDisplayWidget::paintEvent(QPaintEvent *)
{
    paintCard(this);
}

// ---------------------------------------------------------------------------
// ProgressBar / ProgressRing
// ---------------------------------------------------------------------------

ProgressBarWidget::ProgressBarWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_bar = new FluentQt::ProgressBar(this, false);
    layout->addWidget(m_bar, 0, Qt::AlignVCenter);
}

void ProgressBarWidget::apply()
{
    m_bar->setRange(0, std::max(1, integer("maximum")));
    m_bar->setValue(integer("value"));
}

void ProgressBarWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto number = bindingInteger(value)) {
        m_bar->setValue(std::clamp(*number, m_bar->minimum(), m_bar->maximum()));
    }
}

ProgressRingWidget::ProgressRingWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    m_ring = new FluentQt::ProgressRing(this, false);
    m_ring->setTextVisible(true);
    m_ring->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_label = new FluentQt::BodyLabel(this);
    m_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_ring, 1, Qt::AlignHCenter);
    layout->addWidget(m_label);
}

void ProgressRingWidget::apply()
{
    m_label->setText(text("label"));
    m_ring->setRange(0, std::max(1, integer("maximum")));
    m_ring->setValue(integer("value"));
}

void ProgressRingWidget::resizeEvent(QResizeEvent *event)
{
    HmiWidget::resizeEvent(event);
    const int side = std::max(40, std::min(width() - 8, height() - 36));
    m_ring->setFixedSize(side, side);
}

void ProgressRingWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto number = bindingInteger(value)) {
        m_ring->setValue(std::clamp(*number, m_ring->minimum(), m_ring->maximum()));
    }
}

// ---------------------------------------------------------------------------
// ImageDisplay
// ---------------------------------------------------------------------------

ImageWidget::ImageWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(1, 1);
    layout->addWidget(m_image);
}

void ImageWidget::apply()
{
    QString path = text("imagePath");
    if (!path.isEmpty() && QFileInfo(path).isRelative()) {
        path = QDir(assetBaseDir().isEmpty() ? QDir::currentPath() : assetBaseDir()).filePath(path);
    }
    m_pixmap = path.isEmpty() ? QPixmap() : QPixmap(path);
    if (m_pixmap.isNull()) {
        m_image->setPixmap(QPixmap());
        m_image->setText(path.isEmpty() ? tr("未选择图片") : tr("无法加载图片"));
        m_image->setStyleSheet(QStringLiteral("QLabel { color: #888; border: 1px dashed #999; border-radius: 6px; }"));
    } else {
        m_image->setStyleSheet(QString());
        refresh();
    }
}

void ImageWidget::resizeEvent(QResizeEvent *event)
{
    HmiWidget::resizeEvent(event);
    refresh();
}

void ImageWidget::refresh()
{
    if (!m_pixmap.isNull()) {
        QPixmap scaled = m_pixmap.scaled(size() * devicePixelRatioF(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(devicePixelRatioF());
        m_image->setPixmap(scaled);
    }
}

// ---------------------------------------------------------------------------
// GaugeMeter
// ---------------------------------------------------------------------------

GaugeWidget::GaugeWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    repaintOnThemeChange(this);
}

void GaugeWidget::apply()
{
    m_value = real("value");
    update();
}

void GaugeWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto number = bindingFloat(value)) {
        m_value = *number;
        update();
    }
}

void GaugeWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    const QColor fg = dark ? QColor(255, 255, 255) : QColor(26, 26, 26);
    const QColor muted = dark ? QColor(255, 255, 255, 150) : QColor(0, 0, 0, 140);

    const qreal base = std::min(width(), height());
    const qreal stroke = std::max<qreal>(6, base / 14);
    // A pen straddles its path, so the arc keeps half the stroke clear of every edge.
    const qreal side = base - stroke - 8;
    const QRectF arc(QPointF((width() - side) / 2, (height() - side) / 2 + 4), QSizeF(side, side));

    const double lo = real("minimum");
    const double hi = real("maximum");
    const double ratio = hi > lo ? std::clamp((m_value - lo) / (hi - lo), 0.0, 1.0) : 0.0;
    constexpr int startAngle = 225 * 16;
    constexpr int spanAngle = -270 * 16;

    QPen track(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 22), stroke, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(track);
    painter.drawArc(arc, startAngle, spanAngle);
    if (ratio > 0) {
        QPen value(accentColor(), stroke, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(value);
        painter.drawArc(arc, startAngle, int(spanAngle * ratio));
    }

    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(std::max(10, int(side / 5.5)));
    painter.setFont(font);
    painter.setPen(fg);
    const QString number = std::fabs(m_value) >= 1000 || m_value == std::floor(m_value)
                               ? QString::number(m_value, 'f', 0)
                               : QString::number(m_value, 'f', 1);
    painter.drawText(arc, Qt::AlignCenter, number);

    font.setBold(false);
    font.setPixelSize(std::max(9, int(side / 11)));
    painter.setFont(font);
    painter.setPen(muted);
    const QRectF below = arc.adjusted(0, side * 0.32, 0, 0);
    painter.drawText(below, Qt::AlignHCenter | Qt::AlignVCenter, text("unit"));
    const QRectF bottom(arc.left(), arc.bottom() - side * 0.2, arc.width(), side * 0.2);
    painter.drawText(bottom, Qt::AlignHCenter | Qt::AlignTop, text("title"));
}

} // namespace fhmi
