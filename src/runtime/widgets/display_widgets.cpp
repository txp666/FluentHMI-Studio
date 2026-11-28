#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Color.h>
#include <FluentQtWidgets/StyleSheet.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/InfoBadge.h>
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

void paintCardSurface(QWidget *widget, const QString &style)
{
    if (style == QStringLiteral("透明")) { // i18n: value
        return;
    }
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    if (style == QStringLiteral("描边")) { // i18n: value
        painter.setBrush(Qt::NoBrush);
    } else {
        painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    }
    painter.drawRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
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
    if (name == QLatin1String("orange")) {
        return QColor(0xF7, 0x7F, 0x00);
    }
    if (name == QLatin1String("cyan")) {
        return QColor(0x00, 0xB8, 0xD9);
    }
    if (name == QLatin1String("purple")) {
        return QColor(0x9B, 0x51, 0xE0);
    }
    if (name == QLatin1String("white")) {
        return QColor(0xF0, 0xF4, 0xF8);
    }
    if (name == QLatin1String("gray")) {
        return QColor(0x9E, 0x9E, 0x9E);
    }
    return QColor(0x3C, 0xC0, 0x6A);
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
    m_blinkTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this]() {
        m_blinkVisible = !m_blinkVisible;
        update();
    });
    repaintOnThemeChange(this);
}

void LedWidget::apply()
{
    m_multiState = usesMultiStateLed(properties());
    if (m_multiState) {
        m_entries = parseValueText(enumMapping(properties(), enumTables()));
        m_effects = parseLedEffects(prop("ledEffects"));
        showEnumValue(isBound() ? m_reading : prop("stateValue").toVariant());
    } else {
        m_stateLabel.clear();
        m_halfPeriod = flag("blink") ? 450 : 0;
        if (!isBound()) {
            m_on = flag("on");
            m_known = true;
        }
    }
    m_label->setPixelFontSize(std::clamp(integer("fontSize") > 0 ? integer("fontSize") : 14, 10, 64));
    bindingStatusChanged(bindingStatus());
}

void LedWidget::applyBoundValue(const QVariant &value, const BindingEntry &binding)
{
    if (m_multiState) {
        if (m_reading != value || m_reading.metaType() != value.metaType()) {
            showEnumValue(value);
            bindingStatusChanged(bindingStatus());
        }
        return;
    }
    m_reading = value;
    const auto on = bindingLed(value, binding.ledOnValue, binding.ledOffValue);
    const bool knownChanged = m_known != on.has_value();
    m_known = on.has_value();
    if (on)
        m_on = *on;
    updateBlinkTimer();
    if (knownChanged) {
        bindingStatusChanged(bindingStatus());
    } else {
        update();
    }
}

void LedWidget::showEnumValue(const QVariant &value)
{
    m_reading = value;
    const auto key = valueTextKey(value);
    const auto entry = m_entries.constFind(key);
    m_known = entry != m_entries.cend();
    if (m_known)
        m_stateLabel = entry->text;
    else
        m_stateLabel = value.isValid() ? tr("未知状态（%1）").arg(displayString(value)) : tr("数据未知");
    m_stateColor = m_known ? QColor(entry->color) : QColor();
    const auto effect = m_effects.value(key);
    m_on = m_known && effect != u"off";
    m_halfPeriod = effect == u"slow" ? 500 : effect == u"fast" ? 250 : 0;
}

void LedWidget::updateBlinkTimer()
{
    const bool available = !isBound() || bindingStatus() == BindingStatus::Live;
    if (m_halfPeriod > 0 && available && m_known && m_on) {
        if (!m_blinkTimer.isActive() || m_blinkTimer.interval() != m_halfPeriod) {
            m_blinkVisible = true;
            m_blinkTimer.start(m_halfPeriod);
        }
    } else {
        m_blinkTimer.stop();
        m_blinkVisible = true;
    }
}

void LedWidget::bindingStatusChanged(BindingStatus status)
{
    QString detail;
    if (status == BindingStatus::Waiting) {
        detail = tr("待采集");
    } else if (status == BindingStatus::Stale) {
        detail = tr("数据已过期");
    } else if (m_multiState) {
        detail = m_stateLabel;
    } else if (!m_known) {
        detail = tr("数据未知");
    }
    QString label = text("label");
    if (!detail.isEmpty())
        label = label.isEmpty() ? detail : QStringLiteral("%1 · %2").arg(label, detail);
    m_label->setText(label);
    setToolTip(m_multiState ? label : QString());
    setAccessibleDescription(detail);
    updateBlinkTimer();
    updateLayoutGeometry();
    update();
}

void LedWidget::resizeEvent(QResizeEvent *event)
{
    HmiWidget::resizeEvent(event);
    updateLayoutGeometry();
}

qreal LedWidget::lampDiameter() const
{
    const QString pos = text("labelPosition");
    const QString sizeMode = text("ledSize");
    const bool hasText = pos != QStringLiteral("隐藏") && !m_label->text().isEmpty(); // i18n: value
    const int labelH = m_label->fontMetrics().height();

    qreal maxW = std::max<qreal>(0, width() - 6);
    qreal maxH = std::max<qreal>(0, height() - 6);
    if (hasText && pos == QStringLiteral("底部")) { // i18n: value
        maxH = std::max<qreal>(0, height() - labelH - 2);
    } else if (hasText && (pos.isEmpty() || pos == QStringLiteral("右侧"))) { // i18n: value
        maxW = std::max<qreal>(8, width() * 0.65);
    }
    const qreal maxBox = std::min(maxW, maxH);

    if (sizeMode == QStringLiteral("小")) { // i18n: value
        return std::min<qreal>(14, maxBox);
    }
    if (sizeMode == QStringLiteral("标准")) { // i18n: value
        return std::min<qreal>(18, maxBox);
    }
    if (sizeMode == QStringLiteral("大")) { // i18n: value
        return std::min<qreal>(28, maxBox);
    }
    if (sizeMode == QStringLiteral("超大")) { // i18n: value
        return std::min<qreal>(44, maxBox);
    }
    // 随控件缩放 (default): at default 140x30 gives 18px, and scales up smoothly when the widget is enlarged.
    const qreal scaled = hasText && (pos.isEmpty() || pos == QStringLiteral("右侧")) // i18n: value
                             ? (height() - 6) * 0.75 : maxBox * 0.78;
    return std::min(maxBox, std::max<qreal>(12, scaled));
}

void LedWidget::updateLayoutGeometry()
{
    if (!layout() || !m_label) {
        return;
    }
    const QString shape = text("ledShape");
    const QString pos = text("labelPosition");
    const bool hidden = pos == QStringLiteral("隐藏"); // i18n: value
    m_label->setVisible(!hidden);
    if (hidden) {
        return;
    }

    if (shape == QStringLiteral("胶囊灯牌") || pos == QStringLiteral("内部居中")) { // i18n: value
        layout()->setContentsMargins(8, 2, 8, 2);
        m_label->setAlignment(Qt::AlignCenter);
    } else if (pos == QStringLiteral("底部")) { // i18n: value
        const qreal d = lampDiameter();
        const int topSpace = std::min(int(d + 6), std::max(0, height() - m_label->fontMetrics().height() - 2));
        layout()->setContentsMargins(4, topSpace, 4, 2);
        m_label->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    } else {
        const qreal d = lampDiameter();
        const qreal glowRadius = std::min<qreal>(d * 0.75, std::min(width(), height()) / 2.0);
        const int leftMargin = std::max(28, int(std::max(d, glowRadius * 2) + 8));
        layout()->setContentsMargins(leftMargin, 0, 4, 0);
        m_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (m_multiState) {
        // A compact lamp still needs to show the state code when its fixed caption does not fit.
        const auto margins = layout()->contentsMargins();
        const int available = std::max(0, width() - margins.left() - margins.right());
        const auto metrics = m_label->fontMetrics();
        m_label->setText(metrics.horizontalAdvance(toolTip()) <= available ? toolTip()
                         : metrics.elidedText(accessibleDescription(), Qt::ElideMiddle, available));
    }
}

void LedWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QString shape = text("ledShape");
    const QString pos = text("labelPosition");
    const QColor color = m_multiState ? (m_stateColor.isValid() ? m_stateColor : accentColor()) : ledColor(text("color"));
    const bool available = !isBound() || bindingStatus() == BindingStatus::Live;
    const bool lit = available && m_known && m_on && m_blinkVisible;
    const bool dark = isDarkTheme();

    if (shape == QStringLiteral("胶囊灯牌")) { // i18n: value
        const QRectF pill = QRectF(rect()).adjusted(2, 2, -2, -2);
        const qreal radius = std::min(pill.width(), pill.height()) / 2.0;
        if (lit) {
            QColor fill = color;
            fill.setAlpha(dark ? 210 : 230);
            painter.setBrush(fill);
            painter.setPen(QPen(color.lighter(130), 1.5));
        } else {
            painter.setBrush(dark ? QColor(65, 65, 65) : QColor(210, 210, 210));
            painter.setPen(QPen(dark ? QColor(255, 255, 255, 40) : QColor(0, 0, 0, 40), 1));
        }
        painter.drawRoundedRect(pill, radius, radius);
        return;
    }

    const qreal d = lampDiameter();
    if (d <= 0) {
        return;
    }
    const qreal glowRadius = std::min<qreal>(d * 0.78, std::min(width(), height()) / 2.0);
    QPointF center;
    if (pos == QStringLiteral("隐藏") || pos == QStringLiteral("内部居中") || m_label->text().isEmpty()) { // i18n: value
        center = QRectF(rect()).center();
    } else if (pos == QStringLiteral("底部")) { // i18n: value
        const int labelH = m_label->fontMetrics().height();
        const qreal availH = std::max(0, height() - labelH - 2);
        center = QPointF(width() / 2.0, availH / 2.0);
    } else {
        const qreal cx = std::max<qreal>(d / 2.0 + 3, glowRadius + 1);
        center = QPointF(cx, height() / 2.0);
    }
    const QRectF dot(center.x() - d / 2.0, center.y() - d / 2.0, d, d);

    if (lit) {
        QRadialGradient glow(dot.center(), glowRadius);
        QColor halo = color;
        halo.setAlpha(95);
        glow.setColorAt(0.5, halo);
        glow.setColorAt(1, Qt::transparent);
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        if (shape == QStringLiteral("方形") || shape == QStringLiteral("圆角方形")) { // i18n: value
            const qreal expand = glowRadius - d / 2.0;
            const QRectF glowRect = dot.adjusted(-expand, -expand, expand, expand);
            const qreal r = shape == QStringLiteral("圆角方形") ? d * 0.35 : 4.0; // i18n: value
            painter.drawRoundedRect(glowRect, r, r);
        } else {
            painter.drawEllipse(dot.center(), glowRadius, glowRadius);
        }
        painter.setBrush(color);
    } else {
        painter.setBrush(dark ? QColor(70, 70, 70) : QColor(205, 205, 205));
    }

    painter.setPen(QPen(dark ? QColor(255, 255, 255, 45) : QColor(0, 0, 0, 45), std::max<qreal>(1.0, d / 22.0)));
    if (shape == QStringLiteral("方形")) { // i18n: value
        const qreal r = std::max<qreal>(2.0, d * 0.08);
        painter.drawRoundedRect(dot, r, r);
    } else if (shape == QStringLiteral("圆角方形")) { // i18n: value
        const qreal r = std::max<qreal>(4.0, d * 0.26);
        painter.drawRoundedRect(dot, r, r);
    } else {
        painter.drawEllipse(dot);
    }

    // Specular highlight when diameter is large enough
    if (d >= 16) {
        painter.setPen(Qt::NoPen);
        QRadialGradient highlight(dot.left() + d * 0.34, dot.top() + d * 0.32, d * 0.35);
        highlight.setColorAt(0, QColor(255, 255, 255, lit ? 115 : 55));
        highlight.setColorAt(1, Qt::transparent);
        painter.setBrush(highlight);
        if (shape == QStringLiteral("方形") || shape == QStringLiteral("圆角方形")) { // i18n: value
            const qreal r = shape == QStringLiteral("圆角方形") ? d * 0.22 : 2.0; // i18n: value
            painter.drawRoundedRect(dot.adjusted(1, 1, -1, -1), r, r);
        } else {
            painter.drawEllipse(dot.adjusted(1, 1, -1, -1));
        }
    }
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
    m_badge = new FluentQt::InfoBadge(this);
    m_badge->setObjectName(QStringLiteral("numericDisplayBadge"));
    m_badge->setMargin(3);
    m_badge->setMinimumHeight(22);
    m_badge->hide();
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
    layout->addWidget(m_badge);
    layout->addWidget(m_unit);
    layout->addStretch();
    repaintOnThemeChange(this);
    const auto restyle = [this]() { apply(); };
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this, restyle);
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::accentColorChanged, this, restyle);
}

void NumericDisplayWidget::apply()
{
    const bool badge = text("displayStyle") == QStringLiteral("状态徽标"); // i18n: value
    auto *box = static_cast<QBoxLayout *>(layout());
    box->setDirection(badge ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    box->setSpacing(badge ? 6 : 0);
    m_title->setText(text("title"));
    m_title->setVisible(!badge || !m_title->text().isEmpty());
    m_value->setVisible(!badge);
    m_badge->setVisible(badge);
    // An empty unit keeps its line, so the titles of cards with and without one line up.
    m_unit->setText(text("unit"));
    const bool enumeration = text("displayFormat") == QStringLiteral("枚举"); // i18n: value
    m_unit->setVisible(!enumeration && (!badge || !m_unit->text().isEmpty()));
    m_unit->setMinimumHeight(m_unit->fontMetrics().height());
    if (!isLive())
        m_reading = formatDisplayValue(prop("value").toVariant(), text("displayFormat"), text("decimals"));
    m_enumEntries = enumeration ? parseValueText(enumMapping(properties(), enumTables())) : QHash<QString, ValueText>();
    m_value->setPixelFontSize(std::max(8, integer("fontSize"))); // pixels, like the Python designer
    QFont font = m_value->font();
    font.setBold(true);
    m_value->setFont(font);
    showValue(m_reading);
    update();
}

void NumericDisplayWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    m_reading = value;
    showValue(value);
}

void NumericDisplayWidget::bindingStatusChanged(BindingStatus)
{
    showValue(m_reading);
}

void NumericDisplayWidget::showValue(const QVariant &value)
{
    QString label = value.isValid() ? displayString(value) : QString::fromUtf8("—");
    QColor color = accentColor();
    if (text("displayFormat") == QStringLiteral("枚举") && value.isValid()) { // i18n: value
        const auto it = m_enumEntries.constFind(valueTextKey(value));
        if (it != m_enumEntries.cend()) {
            label = it->text;
            if (QColor(it->color).isValid())
                color = QColor(it->color);
        } else {
            if (!text("unknownText").isEmpty())
                label = text("unknownText");
            else if (!text("enumTable").isEmpty())
                label = tr("未知状态（%1）").arg(label);
            if (QColor(text("unknownColor")).isValid())
                color = QColor(text("unknownColor"));
        }
    }
    const bool mutedState = bindingStatus() == BindingStatus::Waiting || bindingStatus() == BindingStatus::Stale;
    if (mutedState) {
        color = isDarkTheme() ? QColor(QStringLiteral("#8a8a8a")) : QColor(QStringLiteral("#909090"));
        if (!text("enumTable").isEmpty() && text("displayFormat") == QStringLiteral("枚举")) // i18n: value
            label = bindingStatus() == BindingStatus::Waiting ? tr("待采集") : tr("数据已过期");
    }
    m_value->setText(label);
    m_value->setTextColor(color, color);
    m_badge->setText(label);
    if (text("displayStyle") == QStringLiteral("状态徽标")) { // i18n: value
        // Enum colors mean foreground colors in both display styles. The badge gets
        // a separate, softly tinted surface rather than using that text color as fill.
        const auto tint = [&color](const QColor &surface, double amount) {
            return QColor(qRound(surface.red() * (1 - amount) + color.red() * amount),
                          qRound(surface.green() * (1 - amount) + color.green() * amount),
                          qRound(surface.blue() * (1 - amount) + color.blue() * amount));
        };
        m_badge->setCustomBackgroundColor(tint(QColor(Qt::white), 0.12), tint(QColor(43, 43, 43), 0.20));
        // InfoBadge's stylesheet sets its text color, so match that selector as Fluent
        // labels do; a palette change alone would be overwritten by the component.
        const QString foreground = QStringLiteral("QLabel[fqw=\"InfoBadge\"] { color: %1; }");
        FluentQt::FluentStyleSheet::setCustomStyleSheet(m_badge, foreground.arg(color.name()),
                                                      foreground.arg(color.lighter(160).name()));
    }
}

void NumericDisplayWidget::paintEvent(QPaintEvent *)
{
    paintCardSurface(this, text("cardStyle"));
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

void GaugeWidget::bindingStatusChanged(BindingStatus)
{
    update();
}

void GaugeWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    const bool mutedState = bindingStatus() == BindingStatus::Waiting || bindingStatus() == BindingStatus::Stale;
    const QColor fg = mutedState ? (dark ? QColor(255, 255, 255, 125) : QColor(0, 0, 0, 115))
                                 : (dark ? QColor(255, 255, 255) : QColor(26, 26, 26));
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
        QPen value(mutedState ? muted : accentColor(), stroke, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(value);
        painter.drawArc(arc, startAngle, int(spanAngle * ratio));
    }

    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(std::max(10, int(side / 5.5)));
    painter.setFont(font);
    painter.setPen(fg);
    // Keep one decimal place while showing the actual reading (even when out of range).
    const QString number = QString::number(m_value, 'f', 1);
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
