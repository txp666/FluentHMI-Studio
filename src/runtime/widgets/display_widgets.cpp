#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Color.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ProgressBar.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QtMath>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fhmi {

bool isDarkTheme()
{
    return FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
}

// The accent as FluentQtWidgets paints it: lighter in dark mode.
QColor accentColor()
{
    return FluentQt::themeColor();
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
    m_value = new FluentQt::BodyLabel(this);
    m_unit = new FluentQt::CaptionLabel(this);
    for (FluentQt::FluentLabelBase *label :
         {static_cast<FluentQt::FluentLabelBase *>(m_title), static_cast<FluentQt::FluentLabelBase *>(m_value),
          static_cast<FluentQt::FluentLabelBase *>(m_unit)}) {
        label->setAlignment(Qt::AlignCenter);
    }
    for (FluentQt::CaptionLabel *label : {m_title, m_unit}) {
        label->setTextColor(QColor(QStringLiteral("#606060")), QColor(QStringLiteral("#b3b3b3")));
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
    // An empty unit keeps its line, so the titles of cards with and without one line up.
    m_unit->setText(text("unit"));
    m_unit->setMinimumHeight(m_unit->fontMetrics().height());
    m_value->setText(text("value"));
    m_value->setPixelFontSize(std::max(8, integer("fontSize"))); // pixels, like the Python designer
    QFont font = m_value->font();
    font.setBold(true);
    m_value->setFont(font);
    m_value->setTextColor(accentColor(), accentColor());
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

QTransform imageTransform(const QSizeF &box, const QSize &image, int rotation, bool mirrorH, bool mirrorV,
                          const QString &scaleMode)
{
    const int theta = ((rotation % 360) + 360) % 360;
    double cos = 1;
    double sin = 0;
    if (theta % 90 == 0) { // exact quarter turns
        static const double quarter[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
        cos = quarter[theta / 90][0];
        sin = quarter[theta / 90][1];
    } else {
        const double radians = qDegreesToRadians(double(theta));
        cos = std::cos(radians);
        sin = std::sin(radians);
    }
    const double c = std::abs(cos);
    const double n = std::abs(sin);
    const double w = box.width();
    const double h = box.height();
    const double iw = image.width();
    const double ih = image.height();
    // The base rect before turning: the picture's own size, or for 拉伸 the box (its sides swapped near a quarter
    // turn, so a turned picture still fills the box).
    double bw = iw;
    double bh = ih;
    if (scaleMode == QStringLiteral("拉伸")) { // i18n: value
        const bool across = theta % 180 > 45 && theta % 180 < 135;
        bw = across ? h : w;
        bh = across ? w : h;
    }
    // Scaled so the turned base rect's bounding box fits the box; 原始大小 never enlarges.
    double k = std::min(w / (bw * c + bh * n), h / (bw * n + bh * c));
    if (scaleMode == QStringLiteral("原始大小")) { // i18n: value
        k = std::min(1.0, k);
    }
    const double sx = (mirrorH ? -1 : 1) * k * bw / iw;
    const double sy = (mirrorV ? -1 : 1) * k * bh / ih;
    // (x, y) -> u = (x - iw/2)·sx, v = (y - ih/2)·sy -> (w/2 + u·cos - v·sin, h/2 + u·sin + v·cos)
    return QTransform(sx * cos, sx * sin, -sy * sin, sy * cos, w / 2 - iw / 2 * sx * cos + ih / 2 * sy * sin,
                      h / 2 - iw / 2 * sx * sin - ih / 2 * sy * cos);
}

ImageWidget::ImageWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_placeholder = new FluentQt::CaptionLabel(this);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setTextColor(QColor(QStringLiteral("#606060")), QColor(QStringLiteral("#b3b3b3")));
    layout->addWidget(m_placeholder);
    repaintOnThemeChange(this);
}

void ImageWidget::apply()
{
    QString path = text("imagePath");
    if (!path.isEmpty() && QFileInfo(path).isRelative()) {
        path = QDir(assetBaseDir().isEmpty() ? QDir::currentPath() : assetBaseDir()).filePath(path);
    }
    m_image = path.isEmpty() ? QImage() : QImage(path);
    m_scaleMode = text("scaleMode");
    m_rotation = integer("rotation");
    m_mirrorH = flag("mirrorH");
    m_mirrorV = flag("mirrorV");
    m_placeholder->setVisible(m_image.isNull());
    if (m_image.isNull()) {
        m_placeholder->setText(path.isEmpty() ? tr("未选择图片") : tr("无法加载图片"));
    }
    update();
}

void ImageWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    if (!m_image.isNull()) {
        painter.setTransform(imageTransform(size(), m_image.size(), m_rotation, m_mirrorH, m_mirrorV, m_scaleMode));
        painter.drawImage(QPointF(0, 0), m_image);
        return;
    }
    const bool dark = isDarkTheme();
    painter.setPen(QPen(dark ? QColor(0x60, 0x60, 0x60) : QColor(0xC8, 0xC8, 0xC8), 1, Qt::DashLine));
    painter.setBrush(dark ? QColor(0x29, 0x29, 0x29) : QColor(0xF5, 0xF5, 0xF5));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
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
    // One decimal always, of the value held within the range, as PyQt UI Designer shows it (0.0, 996.0).
    const QString number = QString::number(std::clamp(m_value, lo, std::max(lo, hi)), 'f', 1);
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
