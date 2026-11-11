#include "runtime/widgets/industrial_widget.h"

#include "core/bit_profile.h"
#include "core/i18n.h"
#include "core/industrial_catalog.h"
#include "runtime/widgets/display_widgets.h"
#include <FluentQtWidgets/Theme.h>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtGui/QPainter>
#include <QtSvg/QSvgRenderer>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace fhmi {
IndustrialWidget::IndustrialWidget(const WidgetMeta &meta, QWidget *parent)
    : HmiWidget(meta, parent), m_definition(industrialDefinition(meta.typeName))
{
    m_timer.setInterval(40);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        updateAnimation();
        if (!m_timer.isActive()) {
            return;
        }
        const double elapsed = m_animationClock.restart() / 1000.0;
        m_phase = std::fmod(m_phase + animationRate() * elapsed, 1);
        if (m_phase < 0) {
            m_phase += 1;
        }
        update();
    });
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this,
            [this] { update(); });
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::accentColorChanged, this,
            [this] { update(); });
}

namespace {
bool numericPayload(const QVariant &v)
{
    return v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Float || v.typeId() == QMetaType::Int ||
           v.typeId() == QMetaType::UInt || v.typeId() == QMetaType::LongLong || v.typeId() == QMetaType::ULongLong;
}
} // namespace
QJsonObject IndustrialWidget::bitProfile() const
{
    return effectiveBitProfile(m_bitProfiles,
                               text("bitProfile").isEmpty() ? QStringLiteral("default") : text("bitProfile"));
}
void IndustrialWidget::setBitProfiles(const QJsonObject &profiles)
{
    m_bitProfiles = profiles;
    if (!isLive() &&
        (typeName() == QLatin1String("StatusWordPanel") || typeName() == QLatin1String("DigitalTimingChart"))) {
        setReading(prop("value").toVariant());
        return;
    }
    const bool wasKnown = m_reading.has_value();
    validateReading();
    if (wasKnown && !m_reading)
        m_dataError = true;
    updateReadingDescription();
    update();
}

void IndustrialWidget::setReading(const QVariant &value)
{
    m_reading = bindingFloat(value);
    if (typeName() == QLatin1String("BmsCellMatrix")) {
        m_cells.clear();
        m_reading.reset();
        if (value.typeId() == QMetaType::QVariantList) {
            const auto cells = value.toList();
            bool valid = !cells.isEmpty();
            long double total = 0;
            for (const auto &cell : cells) {
                const auto number = bindingFloat(cell);
                if (!numericPayload(cell) || !number) {
                    valid = false;
                    break;
                }
                m_cells.append(*number);
                total += *number;
            }
            if (valid)
                m_reading = double(total / cells.size());
            else
                m_cells.clear();
        }
    } else if (typeName() == QLatin1String("DcPowerSupply")) {
        m_supply.clear();
        m_reading.reset();
        if (value.typeId() == QMetaType::QVariantMap) {
            auto data = value.toMap();
            const auto voltage = bindingFloat(data.value(QStringLiteral("voltage"))),
                       current = bindingFloat(data.value(QStringLiteral("current")));
            const QString mode = data.value(QStringLiteral("mode")).toString().trimmed().toUpper();
            if (numericPayload(data.value(QStringLiteral("voltage"))) &&
                numericPayload(data.value(QStringLiteral("current"))) && voltage && current &&
                (mode == QLatin1String("CV") || mode == QLatin1String("CC"))) {
                data.insert(QStringLiteral("mode"), mode);
                m_supply = data;
                m_reading = *voltage;
            }
        }
    }
    validateReading();
    m_dataError = !m_reading;
    updateReadingDescription();
    updateAnimation();
    update();
}

void IndustrialWidget::updateReadingDescription()
{
    const QString description = readingText();
    setToolTip(description);
    if (typeName() == QLatin1String("PressureGauge")) {
        setAccessibleName(text("title"));
        setAccessibleDescription(description);
    }
}

void IndustrialWidget::validateReading()
{
    const auto mode = m_definition.value(QStringLiteral("mode")).toString();
    if (m_reading && (typeName() == QLatin1String("BmsCellMatrix") || typeName() == QLatin1String("DcPowerSupply")))
        return;
    if (m_reading &&
        (typeName() == QLatin1String("StatusWordPanel") || typeName() == QLatin1String("DigitalTimingChart"))) {
        const double maximum = bitProfile().value(QStringLiteral("width")).toInt(8) == 16 ? 65535 : 255;
        if (*m_reading < 0 || *m_reading > maximum || std::floor(*m_reading) != *m_reading)
            m_reading.reset();
        return;
    }
    if (m_reading && typeName() == QLatin1String("RotaryEncoder")) {
        if (std::abs(*m_reading) > 9007199254740991.0 || std::floor(*m_reading) != *m_reading)
            m_reading.reset();
        return;
    }
    if (m_reading && mode != QLatin1String("number")) {
        const double maximum =
            mode == QLatin1String("byte") || mode == QLatin1String("flags")
                ? m_definition.value(QStringLiteral("maximum")).toDouble(mode == QLatin1String("byte") ? 255 : 15)
            : mode == QLatin1String("bits") ? 7
                                            : m_definition.value(QStringLiteral("states")).toArray().size() - 1;
        if (*m_reading < 0 || *m_reading > maximum || std::floor(*m_reading) != *m_reading) {
            m_reading.reset();
        }
    }
    if (m_reading &&
        ((typeName() == QLatin1String("Motor") && text("feedbackMode") == QStringLiteral("运行位") && // i18n: value
          (*m_reading != 0 && *m_reading != 1)) ||
         (typeName() == QLatin1String("Cylinder") &&
          text("feedbackMode") == QStringLiteral("端位状态") && // i18n: value
          (*m_reading != 0 && *m_reading != 1 && *m_reading != 2)))) {
        m_reading.reset();
    }
}

void IndustrialWidget::apply()
{
    if (!isLive()) {
        setReading((typeName() == QLatin1String("BmsCellMatrix") || typeName() == QLatin1String("DcPowerSupply"))
                       ? m_definition.value(QStringLiteral("value")).toVariant()
                       : prop("value").toVariant());
    }
    const bool hadReading = m_reading.has_value();
    validateReading();
    if (hadReading && !m_reading) {
        m_dataError = true;
    }
    updateReadingDescription();
    updateAnimation();
    update();
}

void IndustrialWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    const bool boolean = value.typeId() == QMetaType::Bool;
    setReading(boolean && (typeName() == QLatin1String("RotaryEncoder") ||
                           typeName() == QLatin1String("StatusWordPanel") ||
                           typeName() == QLatin1String("DigitalTimingChart"))
                   ? QVariant()
                   : value);
    if (typeName() == QLatin1String("DigitalTimingChart")) {
        m_timingSamples.append(m_reading ? std::optional<quint32>(quint32(*m_reading)) : std::nullopt);
        while (m_timingSamples.size() > 32)
            m_timingSamples.removeFirst();
    }
    if (typeName() == QLatin1String("RotaryEncoder")) {
        m_encoderDirection = m_reading && m_previousEncoder ? std::optional<int>(*m_reading > *m_previousEncoder   ? 1
                                                                                 : *m_reading < *m_previousEncoder ? -1
                                                                                                                   : 0)
                                                            : std::nullopt;
        m_previousEncoder = m_reading;
    }
    updateReadingDescription();
    update();
}

bool IndustrialWidget::active() const
{
    if (!m_reading) {
        return false;
    }
    if (typeName() == QLatin1String("Pump")) {
        return *m_reading == 1;
    }
    if (typeName() == QLatin1String("AirCompressor")) {
        return *m_reading == 1 || *m_reading == 3;
    }
    return *m_reading != 0;
}

double IndustrialWidget::encoderPulses() const
{
    const auto value = bindingFloat(prop("pulsesPerTurn").toVariant());
    return value && *value > 0 && *value <= 9007199254740991.0 && std::floor(*value) == *value ? *value : 1024;
}

double IndustrialWidget::magnitude() const
{
    const double scale = std::max(std::abs(real("minimum")), std::abs(real("maximum")));
    return m_reading && scale > 0 ? std::clamp(std::abs(*m_reading) / scale, 0.0, 1.0) : 0;
}

double IndustrialWidget::animationRate() const
{
    if (!active()) {
        return 0;
    }
    if (typeName() == QLatin1String("Motor") && text("feedbackMode") == QStringLiteral("运行位")) { // i18n: value
        return 0.55;
    }
    const auto motion = m_definition.value(QStringLiteral("motion")).toString();
    if (motion == QLatin1String("signed")) {
        return (*m_reading < 0 ? -1 : 1) * (0.15 + 0.75 * magnitude());
    }
    if (motion == QLatin1String("run")) {
        return 0.55;
    }
    if (motion == QLatin1String("compressor")) {
        return *m_reading == 3 ? 0.3 : 0.55;
    }
    return 0;
}

bool IndustrialWidget::moving() const { return animationRate() != 0; }

void IndustrialWidget::updateAnimation()
{
    if (isVisible() && !visibleRegion().isEmpty() && isLive() && context()->isActive() && moving()) {
        if (!m_timer.isActive()) {
            m_animationClock.start();
            m_timer.start();
        }
    } else {
        m_timer.stop();
    }
}

void IndustrialWidget::onAttached()
{
    m_reading.reset();
    m_cells.clear();
    m_supply.clear();
    m_timingSamples.clear();
    m_previousEncoder.reset();
    m_encoderDirection.reset();
    setBitProfiles(context()->bitProfiles());
    live(connect(context(), &RuntimeContext::bitProfilesChanged, this,
                 [this] { setBitProfiles(context()->bitProfiles()); }));
    m_dataError = false;
    updateReadingDescription();
    live(connect(context(), &RuntimeContext::activeChanged, this, [this] { updateAnimation(); }));
    updateAnimation();
    update();
}

void IndustrialWidget::onDetached() { m_timer.stop(); }

void IndustrialWidget::showEvent(QShowEvent *event)
{
    HmiWidget::showEvent(event);
    updateAnimation();
}

void IndustrialWidget::hideEvent(QHideEvent *event)
{
    m_timer.stop();
    HmiWidget::hideEvent(event);
}

QString IndustrialWidget::readingText() const
{
    if (!m_reading) {
        return QString::fromUtf8("— · ") + (m_dataError ? tr("数据异常") : tr("待数据"));
    }
    if (typeName() == QLatin1String("StatusWordPanel") || typeName() == QLatin1String("DigitalTimingChart"))
        return QStringLiteral("0x%1").arg(
            QString::number(quint32(*m_reading), 16)
                .rightJustified(bitProfile().value(QStringLiteral("width")).toInt() / 4, QLatin1Char('0'))
                .toUpper());
    if (typeName() == QLatin1String("RotaryEncoder")) {
        const qint64 ppr = qint64(encoderPulses()), count = qint64(*m_reading), magnitude = count < 0 ? -count : count;
        qint64 turns = magnitude / ppr, millis = ((magnitude % ppr) * 1000 + ppr / 2) / ppr;
        if (millis == 1000) {
            ++turns;
            millis = 0;
        }
        const QString turnText = QStringLiteral("%1%2.%3")
                                     .arg(count < 0 ? QStringLiteral("-") : QString())
                                     .arg(turns)
                                     .arg(millis, 3, 10, QLatin1Char('0'));
        const auto direction = !m_encoderDirection       ? tr("方向未知")
                               : *m_encoderDirection > 0 ? tr("正向")
                               : *m_encoderDirection < 0 ? tr("反向")
                                                         : tr("停止");
        return tr("计数 %1 · 圈数 %2 · %3").arg(QString::number(*m_reading, 'f', 0), turnText, direction);
    }
    if (typeName() == QLatin1String("DcPowerSupply"))
        return QStringLiteral("%1 V · %2 A · %3")
            .arg(QString::number(m_supply.value(QStringLiteral("voltage")).toDouble(), 'f', 2),
                 QString::number(m_supply.value(QStringLiteral("current")).toDouble(), 'f', 3),
                 m_supply.value(QStringLiteral("mode")).toString());
    if (typeName() == QLatin1String("BmsCellMatrix")) {
        const auto low = std::min_element(m_cells.begin(), m_cells.end()),
                   high = std::max_element(m_cells.begin(), m_cells.end());
        std::ostringstream spread;
        spread.imbue(std::locale::classic());
        spread << std::fixed << std::setprecision(1) << (static_cast<long double>(*high) - *low) * 1000;
        return tr("最低 C%1 %2 V · 最高 C%3 %4 V · Δ %5 mV")
            .arg(std::distance(m_cells.begin(), low) + 1)
            .arg(*low, 0, 'f', 3)
            .arg(std::distance(m_cells.begin(), high) + 1)
            .arg(*high, 0, 'f', 3)
            .arg(QString::fromStdString(spread.str()));
    }
    if (m_definition.value(QStringLiteral("mode")).toString() == QLatin1String("byte")) {
        return QStringLiteral("0x%1").arg(
            QString::number(int(*m_reading), 16).rightJustified(2, QLatin1Char('0')).toUpper());
    }
    if (m_definition.value(QStringLiteral("mode")).toString() == QLatin1String("flags")) {
        const bool english = language() == kLanguageEnglish;
        const int bits = int(*m_reading);
        if (bits == 0) {
            return m_definition.value(english ? QStringLiteral("clearLabelEn") : QStringLiteral("clearLabel"))
                .toString();
        }
        const auto labels = m_definition.value(english ? QStringLiteral("flagsEn") : QStringLiteral("flags")).toArray();
        QStringList active;
        for (int bit = 0; bit < labels.size() && bit < 8; ++bit) {
            if (bits & (1 << bit))
                active.append(labels.at(bit).toString());
        }
        return active.join(english ? QStringLiteral(", ") : QString::fromUtf8("、")); // i18n: ignore
    }
    if (m_definition.value(QStringLiteral("mode")).toString() != QLatin1String("number")) {
        const auto states =
            m_definition.value(language() == kLanguageEnglish ? QStringLiteral("statesEn") : QStringLiteral("states"))
                .toArray();
        return states.at(int(*m_reading)).toString();
    }
    if (typeName() == QLatin1String("Motor") && text("feedbackMode") == QStringLiteral("运行位")) { // i18n: value
        return *m_reading == 1 ? tr("运行") : tr("停止");
    }
    if (typeName() == QLatin1String("Cylinder") && text("feedbackMode") == QStringLiteral("端位状态")) { // i18n: value
        return *m_reading == 0 ? tr("两端未到位") : *m_reading == 1 ? tr("缩回") : tr("伸出");
    }
    QString reading = QLocale::c().toString(*m_reading, 'f', std::clamp(integer("decimals"), 0, 6));
    if (!text("unit").isEmpty()) {
        reading += QLatin1Char(' ') + text("unit");
    }
    if (typeName() == QLatin1String("PowerMeter")) {
        reading += QString::fromUtf8(" · ") + (*m_reading < 0 ? tr("输出") : *m_reading > 0 ? tr("输入") : tr("停止"));
    } else if (m_definition.value(QStringLiteral("motion")).toString() == QLatin1String("signed") ||
               typeName() == QLatin1String("Tachometer")) {
        const bool flow = typeName() == QLatin1String("FlowMeter") || typeName() == QLatin1String("FlowPipe");
        reading += QString::fromUtf8(" · ") + (*m_reading < 0   ? tr("反向")
                                               : *m_reading > 0 ? tr("正向")
                                               : flow           ? tr("零流量")
                                                                : tr("停止"));
    }
    const QString limit = limitText(*m_reading);
    if (!limit.isEmpty()) {
        reading += QString::fromUtf8(" · ") + limit;
    }
    return reading;
}

QString IndustrialWidget::limitText(double value) const
{
    if (!limitsEnabled()) {
        return {};
    }
    if (value <= real("lowLimit")) {
        return tr("低于限值");
    }
    return value >= real("highLimit") ? tr("高于限值") : QString{};
}

QHash<QString, double> IndustrialWidget::svgDrivers() const
{
    const double value = m_reading.value_or(0);
    const bool known = m_reading.has_value();
    const double lo = real("minimum"), hi = real("maximum");
    double level = known && hi > lo
                       ? double(std::clamp((static_cast<long double>(value) - lo) / (static_cast<long double>(hi) - lo),
                                           0.0L, 1.0L))
                       : 0;
    const bool cylinderEnds =
        typeName() == QLatin1String("Cylinder") && text("feedbackMode") == QStringLiteral("端位状态"); // i18n: value
    if (cylinderEnds) {
        level = known && value == 2 ? 1 : 0;
    }
    QHash<QString, double> drivers{{"level", level},
                                   {"meniscus", std::min(1.0, 8 * level)},
                                   {"partial", known && level > 0 && level < 1 ? 1.0 : 0},
                                   {"magnitude", magnitude()},
                                   {"phase", m_phase},
                                   {"known", known ? 1.0 : 0},
                                   {"active", active() ? 1.0 : 0},
                                   {"positive", known && value > 0 ? 1.0 : 0},
                                   {"negative", known && value < 0 ? 1.0 : 0},
                                   {"zero", known && value == 0 ? 1.0 : 0}};
    for (int i = 0; i < 8; ++i) {
        drivers.insert(QStringLiteral("state%1").arg(i), known && value == i ? 1 : 0);
    }
    const int bits = known && value >= 0 && value <= 255 ? int(value) : 0;
    for (int i = 0; i < 8; ++i) {
        drivers.insert(QStringLiteral("bit%1").arg(i), known && (bits & (1 << i)) ? 1 : 0);
    }
    for (int i = 1; i <= 12; ++i) {
        drivers.insert(QStringLiteral("seg%1").arg(i), known && level >= i / 12.0 ? 1 : 0);
    }
    const bool limits = limitsEnabled();
    const auto mark = [&](double threshold) {
        return hi > lo
                   ? double(std::clamp((static_cast<long double>(threshold) - lo) / (static_cast<long double>(hi) - lo),
                                       0.0L, 1.0L))
                   : 0;
    };
    drivers.insert(QStringLiteral("limitsOn"), limits ? 1 : 0);
    drivers.insert(QStringLiteral("lowMark"), limits ? mark(real("lowLimit")) : 0);
    drivers.insert(QStringLiteral("highMark"), limits ? mark(real("highLimit")) : 0);
    drivers.insert(QStringLiteral("lowAlarm"), known && limits && value <= real("lowLimit") ? 1 : 0);
    drivers.insert(QStringLiteral("highAlarm"), known && limits && value >= real("highLimit") ? 1 : 0);
    drivers.insert(QStringLiteral("straight"), text("pipeShape") == QStringLiteral("直管") ? 1 : 0); // i18n: value
    drivers.insert(QStringLiteral("elbow"), text("pipeShape") == QStringLiteral("直管") ? 0 : 1);    // i18n: value
    drivers.insert(QStringLiteral("mirrored"),
                   text("pipeDirection") == QStringLiteral("镜像布局") ? 1 : 0); // i18n: value
    drivers.insert(QStringLiteral("thickness"), std::clamp((real("pipeWidth") - 8) / 18, 0.0, 1.0));
    drivers.insert(QStringLiteral("pistonKnown"), known && (!cylinderEnds || value == 1 || value == 2) ? 1 : 0);
    return drivers;
}

bool IndustrialWidget::limitsEnabled() const
{
    const auto low = bindingFloat(prop("lowLimit").toVariant());
    const auto high = bindingFloat(prop("highLimit").toVariant());
    return m_definition.value(QStringLiteral("limits")).toArray().size() == 2 && flag("showLimits") && low && high &&
           *low < *high;
}

QByteArray IndustrialWidget::renderedSvg() const
{
    QString svg = m_definition.value(QStringLiteral("svg")).toString();
    const auto replace = [&svg](const QString &name, const QString &value) {
        svg.replace(QStringLiteral("{{%1}}").arg(name), value);
    };
    const double value = m_reading.value_or(0);
    const bool known = m_reading.has_value();
    const double lo = real("minimum"), hi = real("maximum");
    const auto drivers = svgDrivers();
    const auto templateSlots = m_definition.value(QStringLiteral("slots")).toObject();
    for (auto it = templateSlots.begin(); it != templateSlots.end(); ++it) {
        const auto spec = it.value().toArray();
        const double from = spec.at(1).toDouble(), to = spec.at(2).toDouble();
        replace(it.key(), QString::number(from + (to - from) * drivers.value(spec.at(0).toString()), 'g', 12));
    }
    const bool dark = isDarkTheme();
    const QHash<QString, QString> colors{{"ink", dark ? "#e8edf4" : "#273549"},
                                         {"edge", dark ? "#718198" : "#708297"},
                                         {"muted", dark ? "#a4b3c5" : "#64748b"},
                                         {"body", dark ? "#39495e" : "#dbe4ed"},
                                         {"surface", dark ? "#172332" : "#f4f7fa"},
                                         {"accent", accentColor().name()},
                                         {"liquid", "#29a9d6"},
                                         {"warm", "#f49b38"},
                                         {"danger", "#e55252"},
                                         {"green", "#3dbd84"},
                                         {"yellow", "#e8b643"},
                                         {"grain", "#cfa568"}};
    for (auto it = colors.begin(); it != colors.end(); ++it) {
        replace(it.key(), it.value());
    }
    const QString reading = !known ? QString::fromUtf8("—")
                            : (m_definition.value(QStringLiteral("mode")).toString() == QLatin1String("byte") ||
                               m_definition.value(QStringLiteral("mode")).toString() == QLatin1String("flags"))
                                ? readingText()
                                : QLocale::c().toString(value, 'f', std::clamp(integer("decimals"), 0, 6));
    replace(QStringLiteral("reading"), reading.toHtmlEscaped());
    replace(QStringLiteral("degrees"), QString::number(known ? value : 0, 'g', 17));
    const qint64 ppr = qint64(encoderPulses());
    qint64 remainder = known && typeName() == QLatin1String("RotaryEncoder") ? qint64(value) % ppr : 0;
    if (remainder < 0)
        remainder += ppr;
    replace(QStringLiteral("encoderAngle"), QString::number(double(remainder) / ppr * 360, 'g', 17));
    replace(QStringLiteral("forwardOn"),
            m_encoderDirection && *m_encoderDirection > 0 ? QStringLiteral("1") : QStringLiteral("0"));
    replace(QStringLiteral("reverseOn"),
            m_encoderDirection && *m_encoderDirection < 0 ? QStringLiteral("1") : QStringLiteral("0"));
    replace(QStringLiteral("voltageText"),
            known ? QString::number(m_supply.value(QStringLiteral("voltage")).toDouble(), 'f', 2)
                  : QString::fromUtf8("—"));
    replace(QStringLiteral("currentText"),
            known ? QString::number(m_supply.value(QStringLiteral("current")).toDouble(), 'f', 3)
                  : QString::fromUtf8("—"));
    replace(QStringLiteral("cvOn"), known && m_supply.value(QStringLiteral("mode")).toString() == QLatin1String("CV")
                                        ? QStringLiteral("1")
                                        : QStringLiteral("0"));
    replace(QStringLiteral("ccOn"), known && m_supply.value(QStringLiteral("mode")).toString() == QLatin1String("CC")
                                        ? QStringLiteral("1")
                                        : QStringLiteral("0"));
    if (typeName() == QLatin1String("PressureGauge")) {
        const double size = std::min(28.0, 180.0 / reading.size());
        replace(QStringLiteral("readingSize"), QString::number(size, 'g', 12));
    }
    replace(QStringLiteral("unit"), text("unit").toHtmlEscaped());
    replace(QStringLiteral("minLabel"), QString::number(lo, 'g', 6));
    replace(QStringLiteral("maxLabel"), QString::number(hi, 'g', 6));
    QString items;
    const auto itemTemplate = m_definition.value(QStringLiteral("itemSvg")).toString();
    const auto item = [&](const QHash<QString, QString> &local) {
        QString cell = itemTemplate;
        for (auto it = colors.begin(); it != colors.end(); ++it)
            cell.replace(QStringLiteral("{{%1}}").arg(it.key()), it.value());
        for (auto it = local.begin(); it != local.end(); ++it)
            cell.replace(QStringLiteral("{{%1}}").arg(it.key()), it.value());
        return cell;
    };
    const auto n = [](double v) { return QString::number(v, 'g', 14); };
    const auto nameSize = [](const QString &name, double base, double room) {
        double units = 0;
        for (const auto code : name.toUcs4())
            units += code < 128 && code != 'M' && code != 'W' && code != '@' && code != '%' ? .68 : 1;
        return std::min(base, room / std::max(1.0, units));
    };
    if (typeName() == QLatin1String("StatusWordPanel")) {
        const auto profile = bitProfile();
        const int width = profile.value(QStringLiteral("width")).toInt();
        const int columns = width == 16 ? 4 : 2;
        const auto bits = profile.value(QStringLiteral("bits")).toArray();
        for (int i = 0; i < width; ++i) {
            const auto bit = bits.at(i).toObject();
            items += item({{QStringLiteral("x"), n(8 + (i % columns) * 304.0 / columns)},
                           {QStringLiteral("y"), n(8 + (i / columns) * 36)},
                           {QStringLiteral("w"), n(304.0 / columns - 4)},
                           {QStringLiteral("nameSize"),
                            n(nameSize(bit.value(QStringLiteral("name")).toString(), 11, 304.0 / columns - 4 - 28))},
                           {QStringLiteral("name"), bit.value(QStringLiteral("name"))
                                                        .toString()
                                                        .toHtmlEscaped()
                                                        .replace(QLatin1Char('{'), QStringLiteral("&#123;"))
                                                        .replace(QLatin1Char('}'), QStringLiteral("&#125;"))},
                           {QStringLiteral("color"), bit.value(QStringLiteral("color")).toString()},
                           {QStringLiteral("lit"),
                            known && (quint32(value) & (1u << i)) ? QStringLiteral("1") : QStringLiteral("0")}});
        }
    } else if (typeName() == QLatin1String("DigitalTimingChart")) {
        const auto profile = bitProfile();
        const int width = profile.value(QStringLiteral("width")).toInt();
        const double h = 144.0 / width, step = 228.0 / 32;
        const auto bits = profile.value(QStringLiteral("bits")).toArray();
        const auto samples = !isLive() && m_timingSamples.isEmpty() && m_reading
                                 ? QList<std::optional<quint32>>{quint32(*m_reading)}
                                 : m_timingSamples;
        for (int bit = 0; bit < width; ++bit) {
            QString trace;
            bool previous = false;
            for (int i = 0; i < samples.size(); ++i) {
                const auto sample = samples.at(i);
                if (!sample) {
                    previous = false;
                    continue;
                }
                const double x = 82 + (32 - samples.size() + i) * step, y = (*sample & (1u << bit)) ? .2 * h : .8 * h;
                if (!previous)
                    trace += QStringLiteral("M %1 %2 ").arg(n(x), n(y));
                else
                    trace += QStringLiteral("V %1 ").arg(n(y));
                trace += QStringLiteral("H %1 ").arg(n(x + step));
                previous = true;
            }
            if (trace.isEmpty())
                trace = QStringLiteral("M 82 %1").arg(n(.8 * h));
            const auto entry = bits.at(bit).toObject();
            items += item({{QStringLiteral("y"), n(8 + bit * h)},
                           {QStringLiteral("h"), n(h)},
                           {QStringLiteral("labelY"), n(.70 * h)},
                           {QStringLiteral("fontSize"),
                            n(nameSize(entry.value(QStringLiteral("name")).toString(), std::min(11.0, .77 * h), 70))},
                           {QStringLiteral("name"), entry.value(QStringLiteral("name"))
                                                        .toString()
                                                        .toHtmlEscaped()
                                                        .replace(QLatin1Char('{'), QStringLiteral("&#123;"))
                                                        .replace(QLatin1Char('}'), QStringLiteral("&#125;"))},
                           {QStringLiteral("color"), entry.value(QStringLiteral("color")).toString()},
                           {QStringLiteral("trace"), trace}});
        }
    } else if (typeName() == QLatin1String("BmsCellMatrix") && known) {
        const int count = m_cells.size(), columns = std::min(count, int(std::ceil(std::sqrt(2.0 * count)))),
                  rows = (count + columns - 1) / columns;
        const double cellW = 304.0 / columns, cellH = 144.0 / rows, gap = std::min(2.0, .1 * std::min(cellW, cellH)),
                     w = cellW - gap, h = cellH - gap;
        const auto low = std::min_element(m_cells.begin(), m_cells.end()),
                   high = std::max_element(m_cells.begin(), m_cells.end());
        for (int i = 0; i < count; ++i) {
            const auto value = m_cells.at(i);
            const QString outline = *low == *high    ? colors.value(QStringLiteral("muted"))
                                    : value == *low  ? colors.value(QStringLiteral("liquid"))
                                    : value == *high ? colors.value(QStringLiteral("warm"))
                                                     : colors.value(QStringLiteral("muted"));
            items += item({{QStringLiteral("x"), n(8 + (i % columns) * cellW)},
                           {QStringLiteral("y"), n(8 + (i / columns) * cellH)},
                           {QStringLiteral("w"), n(w)},
                           {QStringLiteral("h"), n(h)},
                           {QStringLiteral("cx"), n(w / 2)},
                           {QStringLiteral("nameY"), n(.39 * h)},
                           {QStringLiteral("valueY"), n(.79 * h)},
                           {QStringLiteral("nameSize"), n(std::min({11.0, .30 * h, .20 * w}))},
                           {QStringLiteral("valueSize"), n(std::min({16.0, .33 * h, .23 * w}))},
                           {QStringLiteral("name"), QStringLiteral("C%1").arg(i + 1)},
                           {QStringLiteral("valueText"), QString::number(value, 'f', 3)},
                           {QStringLiteral("outline"), outline}});
        }
    }
    replace(QStringLiteral("items"), items);
    return svg.toUtf8();
}

void IndustrialWidget::paintEvent(QPaintEvent *)
{
    // Scrolling back into a viewport repaints a child without showing it again.
    updateAnimation();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(isDarkTheme() ? QColor("#e8edf4") : QColor("#273549"));
    painter.drawText(QRect(4, 3, width() - 8, 22), Qt::AlignCenter,
                     text("title").isEmpty() ? m_definition.value(QStringLiteral("name")).toString() : text("title"));
    QSvgRenderer renderer(renderedSvg());
    if (renderer.isValid()) {
        const QRectF available(4, 26, width() - 8, std::max(1, height() - 53));
        QSizeF size = renderer.viewBoxF().size();
        size.scale(available.size(), Qt::KeepAspectRatio);
        renderer.render(&painter, QRectF(available.center() - QPointF(size.width() / 2, size.height() / 2), size));
    }
    const QString footer =
        typeName() == QLatin1String("PressureGauge") && m_reading ? limitText(*m_reading) : readingText();
    painter.drawText(QRect(4, height() - 25, width() - 8, 22), Qt::AlignCenter,
                     painter.fontMetrics().elidedText(footer, Qt::ElideRight, width() - 8));
}
} // namespace fhmi
