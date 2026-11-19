#include "runtime/widgets/digital_timing_widget.h"

#include "comm/can/can_link.h"
#include "core/bit_profile.h"
#include "core/project.h"
#include "runtime/app_dialog.h"
#include "runtime/runtime_context.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/chart_viewport.h"
#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Color.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QUuid>
#include <QtGui/QDesktopServices>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <deque>
#include <iterator>

namespace fhmi {
namespace {

constexpr int kDefaultWindowSeconds = 10;
constexpr int kHistorySeconds = 3600;
constexpr int kHistoryTransitions = 10000;
constexpr qreal kMinimumSpan = 0.02;

std::optional<bool> digitalLevel(const BindingEntry &binding, const QVariant &raw)
{
    const auto value = bindingFloat(binding.evaluateStrict(raw));
    return value && (*value == 0 || *value == 1) ? std::optional<bool>(*value != 0) : std::nullopt;
}

QString levelText(std::optional<bool> level)
{
    return level ? QString::number(*level) : QStringLiteral("—");
}

QJsonArray namedChannels(const QStringList &names)
{
    QJsonArray channels;
    for (int i = 0; i < names.size(); ++i)
        channels.append(QJsonObject{{QStringLiteral("name"), names.at(i)},
                                    {QStringLiteral("color"), chartSeriesColor(i).name()}});
    return channels;
}

qreal tickInterval(qreal span, qreal width)
{
    const qreal desired = span / std::max(2, int(width / 72));
    const qreal magnitude = std::pow(10.0, std::floor(std::log10(desired)));
    for (const qreal multiplier : {1.0, 2.0, 5.0}) {
        if (desired <= multiplier * magnitude)
            return multiplier * magnitude;
    }
    return 10 * magnitude;
}

} // namespace

class DigitalTimingPlot : public ChartViewport
{
  public:
    explicit DigitalTimingPlot(QWidget *parent) : ChartViewport(parent)
    {
        setValueTagsVisible(true); // right gutter for each bit's 0/1 readout
        setCrosshairVisible(false);
        setAutoYRange(false);
        connect(this, &RealtimePlotWidget::rangeChanged, this, [this] {
            if (m_settingRange)
                return;
            if (m_recording) {
                updateRange();
                if (viewChanged)
                    viewChanged();
            } else if (autoScroll())
                setFollowing(true);
            else if (xMinimum() != m_left || xMaximum() != m_right)
                setFollowing(false);
        });
        cursorMoved = [this](qreal) { setFollowing(false); };
        cursorCleared = [this] {
            if (viewChanged)
                viewChanged();
        };
        updateRange();
    }

    std::function<void()> viewChanged;

    void setChannels(const QJsonArray &bits, quint16 visible, bool fitNames)
    {
        m_channels.clear();
        m_labelWidth = 58;
        for (int i = 0; i < bits.size(); ++i) {
            if (visible & (1u << i)) {
                const auto entry = bits.at(i).toObject();
                const auto name = entry.value(QStringLiteral("name")).toString();
                m_channels.append({quint16(1u << i), name,
                                   QColor(entry.value(QStringLiteral("color")).toString())});
                if (fitNames)
                    m_labelWidth = std::max(m_labelWidth, fontMetrics().horizontalAdvance(name) + 20);
            }
        }
        update();
    }

    void resetHistory()
    {
        m_samples.clear();
        m_time = 0;
        setFollowing(true);
    }

    bool append(const TimingSample &sample)
    {
        if (!m_samples.empty() && m_samples.back().levels == sample.levels)
            return false;
        m_samples.push_back(sample);
        if (m_samples.size() > kHistoryTransitions)
            m_samples.pop_front();
        return true;
    }

    TimingLevels currentLevels() const { return m_samples.empty() ? TimingLevels{} : m_samples.back().levels; }

    const TimingRecording *recording() const { return m_recording ? &*m_recording : nullptr; }

    void setRecording(std::optional<TimingRecording> recording)
    {
        m_recording = std::move(recording);
        if (m_recording) {
            hideCursor();
            const qreal start = oldestTime();
            setTimeRange(start, start + recordingSpan(m_windowSeconds));
        } else {
            setFollowing(true);
        }
    }

    void advance(qreal seconds)
    {
        m_time = seconds;
        // Retain the level that was already active at the start of the recording window.
        while (m_samples.size() > 1 && m_samples[1].seconds <= m_time - kHistorySeconds)
            m_samples.pop_front();
        if (m_recording)
            return;
        updateRange();
        if (viewChanged)
            viewChanged();
        update();
    }

    void setWindow(int seconds)
    {
        m_windowSeconds = seconds;
        if (m_recording) {
            updateRange(seconds);
            update();
            return;
        }
        setFollowing(true);
    }

    bool following() const { return !m_pausedAt; }
    bool hasLiveSamples() const { return !m_samples.empty(); }
    std::optional<qreal> inspectionTime() const { return cursorPosition(); }

    TimingLevels displayedLevels() const
    {
        const qreal time = cursorPosition().value_or(std::min(xMaximum(), displayedEnd()));
        if (time < oldestTime() || time > displayedEnd())
            return {};
        const auto after = samplesAfter(time);
        return after == samples().begin() ? TimingLevels{} : std::prev(after)->levels;
    }

    QStringList describeChannels(TimingLevels levels) const
    {
        QStringList descriptions;
        for (const Channel &channel : m_channels)
            descriptions.append(channel.name + QLatin1Char(' ') + levelText(levels.level(channel.mask)));
        return descriptions;
    }

    void setFollowing(bool enabled)
    {
        if (m_recording) {
            if (viewChanged)
                viewChanged();
            return;
        }
        if (enabled) {
            m_pausedAt.reset();
            hideCursor();
        } else if (following()) {
            m_pausedAt = m_time;
        }
        updateRange();
        if (viewChanged)
            viewChanged();
        update();
    }

  protected:
    QRectF plotArea() const override
    {
        const QRectF base = ChartViewport::plotArea();
        const qreal left = std::clamp<qreal>(m_labelWidth, base.left(), std::max(base.left(), width() * 0.3));
        return base.adjusted(left - base.left(), 0, 0, 0);
    }

    void paintEvent(QPaintEvent *) override;

    void mousePressEvent(QMouseEvent *event) override
    {
        // The analog context menu's Y scale and data-series actions do not apply to fixed bit lanes.
        if (event->button() == Qt::RightButton)
            event->accept();
        else
            ChartViewport::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override { QWidget::contextMenuEvent(event); }

  private:
    struct Channel
    {
        quint16 mask;
        QString name;
        QColor color;
    };

    std::deque<TimingSample>::const_iterator samplesAfter(qreal time) const
    {
        return std::upper_bound(samples().begin(), samples().end(), time,
                                [](qreal time, const TimingSample &sample) { return time < sample.seconds; });
    }

    const std::deque<TimingSample> &samples() const { return m_recording ? m_recording->samples : m_samples; }
    qreal oldestTime() const
    {
        return m_recording ? samples().front().seconds : std::max<qreal>(0, m_time - kHistorySeconds);
    }
    qreal displayedEnd() const { return m_recording ? samples().back().seconds : m_pausedAt.value_or(m_time); }
    qreal recordingSpan(qreal span) const
    {
        return std::clamp(span, kMinimumSpan, std::max(kMinimumSpan, displayedEnd() - oldestTime()));
    }
    void updateRange(qreal requestedSpan = 0);
    void setTimeRange(qreal start, qreal end);
    void drawTrace(QPainter &painter, const QRectF &plot, const QRectF &lane, const Channel &channel) const;

    QList<Channel> m_channels;
    std::deque<TimingSample> m_samples;
    std::optional<TimingRecording> m_recording;
    std::optional<qreal> m_pausedAt;
    qreal m_time = 0;
    qreal m_left = 0;
    qreal m_right = kDefaultWindowSeconds;
    int m_windowSeconds = kDefaultWindowSeconds;
    int m_labelWidth = 58;
    bool m_settingRange = false;
};

void DigitalTimingPlot::updateRange(qreal requestedSpan)
{
    if (m_recording) {
        const qreal span = recordingSpan(requestedSpan > 0 ? requestedSpan : xMaximum() - xMinimum());
        const qreal start = std::clamp(xMinimum(), oldestTime(), std::max(oldestTime(), displayedEnd() - span));
        setTimeRange(start, start + span);
        return;
    }
    const qreal span =
        following() ? m_windowSeconds : std::clamp(xMaximum() - xMinimum(), kMinimumSpan, qreal(kHistorySeconds));
    const qreal lastStart = std::max<qreal>(oldestTime(), displayedEnd() - span);
    const qreal start = following() ? lastStart : std::clamp(xMinimum(), oldestTime(), lastStart);
    setTimeRange(start, start + span);
}

void DigitalTimingPlot::setTimeRange(qreal start, qreal end)
{
    m_left = start;
    m_right = end;
    const QScopedValueRollback guard(m_settingRange, true);
    setXRange(m_left, m_right);
}

void DigitalTimingPlot::drawTrace(QPainter &painter, const QRectF &plot, const QRectF &lane,
                                  const Channel &channel) const
{
    const qreal from = std::max(oldestTime(), xMinimum());
    const qreal to = std::min(displayedEnd(), xMaximum());
    if (samples().empty() || from > to)
        return;

    const auto y = [&](bool high) { return lane.top() + (high ? 0.25 : 0.75) * lane.height(); };
    auto it = samplesAfter(from);
    auto current = it == samples().begin() ? std::nullopt : std::prev(it)->levels.level(channel.mask);
    QPainterPath trace;
    if (current)
        trace.moveTo(toPixelX(from, plot), y(*current));
    for (; it != samples().end() && it->seconds <= to; ++it) {
        const auto next = it->levels.level(channel.mask);
        if (next == current)
            continue;
        const qreal x = toPixelX(it->seconds, plot);
        if (current)
            trace.lineTo(x, y(*current));
        if (next) {
            if (current)
                trace.lineTo(x, y(*next));
            else
                trace.moveTo(x, y(*next));
        }
        current = next;
    }
    if (current)
        trace.lineTo(toPixelX(to, plot), y(*current));
    painter.setPen(QPen(channel.color, 1.6));
    painter.drawPath(trace);
}

void DigitalTimingPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    const QColor panel(dark ? QStringLiteral("#202020") : QStringLiteral("#ffffff"));
    const QColor textColor(dark ? QStringLiteral("#e8edf4") : QStringLiteral("#273549"));
    const QColor grid = dark ? QColor(255, 255, 255, 33) : QColor(0, 0, 0, 24);
    const QRectF plot = plotArea();
    painter.setPen(Qt::NoPen);
    painter.setBrush(panel);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    painter.setBrush(Qt::NoBrush);
    if (plot.width() < 2 || plot.height() < 2)
        return;

    painter.fillRect(plot, dark ? QColor(22, 22, 22) : QColor(248, 250, 252));
    const qreal laneHeight = plot.height() / std::max<qsizetype>(1, m_channels.size());
    for (int i = 1; i < m_channels.size(); i += 2)
        painter.fillRect(QRectF(plot.left(), plot.top() + i * laneHeight, plot.width(), laneHeight),
                         dark ? QColor(29, 29, 29) : QColor(241, 245, 250));

    const qreal interval = tickInterval(std::max(kMinimumSpan, xMaximum() - xMinimum()), plot.width());
    const int decimals = std::max(0, int(std::ceil(-std::log10(interval))));
    const qreal firstTick = std::ceil(xMinimum() / interval) * interval;
    const int maxTicks = int(plot.width() / 36) + 3;
    for (int i = 0; i < maxTicks; ++i) {
        const qreal tick = firstTick + i * interval;
        if (tick > xMaximum() + interval * 1e-6)
            break;
        const qreal x = toPixelX(tick, plot);
        painter.setPen(grid);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(textColor);
        painter.drawText(QRectF(x - 35, plot.bottom() + 2, 70, 18), Qt::AlignCenter,
                         QLocale().toString(tick, 'f', decimals) + QStringLiteral(" s"));
    }

    const auto selected = displayedLevels();
    QFont labelFont = font();
    labelFont.setPixelSize(std::clamp(int(laneHeight) - 2, 8, 13));
    painter.setFont(labelFont);
    for (int i = 0; i < m_channels.size(); ++i) {
        const auto &channel = m_channels.at(i);
        const QRectF lane(plot.left(), plot.top() + i * laneHeight, plot.width(), laneHeight);
        painter.setPen(grid);
        painter.drawLine(lane.bottomLeft(), lane.bottomRight());
        painter.fillRect(QRectF(6, lane.center().y() - 4, 3, 8), channel.color);
        painter.setPen(textColor);
        const QRectF nameRect(13, lane.top(), plot.left() - 16, laneHeight);
        painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(channel.name, Qt::ElideRight, int(nameRect.width())));
        if (areValueTagsVisible())
            painter.drawText(QRectF(plot.right(), lane.top(), width() - plot.right(), laneHeight), Qt::AlignCenter,
                             levelText(selected.level(channel.mask)));
        painter.save();
        painter.setClipRect(lane);
        drawTrace(painter, plot, lane, channel);
        painter.restore();
    }
    if (m_channels.isEmpty()) {
        painter.setPen(textColor);
        painter.drawText(plot, Qt::AlignCenter, tr("请选择通道"));
    }
    drawCursor(painter, plot, FluentQt::themeColor());
}

DigitalTimingChartWidget::DigitalTimingChartWidget(const WidgetMeta &meta, QWidget *parent)
    : HmiWidget(meta, parent), m_plot(new DigitalTimingPlot(this)), m_title(new FluentQt::StrongBodyLabel(this)),
      m_readout(new FluentQt::CaptionLabel(m_plot)), m_window(new FluentQt::ComboBox(this)),
      m_channels(new FluentQt::PushButton(tr("通道"), this)), m_follow(new FluentQt::PushButton(this)),
      m_export(new FluentQt::ToolButton(FluentQt::icon(FluentQt::FluentIcon::Save), this)),
      m_import(new FluentQt::ToolButton(FluentQt::icon(FluentQt::FluentIcon::Folder), this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    layout->setContentsMargins(4, 3, 4, 3);
    layout->setSpacing(4);
    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(6);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbar->addWidget(m_title, 1);
    m_window->setObjectName(QStringLiteral("timingWindow"));
    m_window->setAccessibleName(tr("时间窗"));
    for (int seconds : {2, 5, kDefaultWindowSeconds, 30, 60, 120})
        m_window->addItem(tr("%1 秒").arg(seconds), {}, seconds);
    m_window->addItem(tr("自定义…"), {}, 0);
    m_window->setCurrentIndex(m_window->findData(kDefaultWindowSeconds));
    toolbar->addWidget(m_window);
    toolbar->addWidget(m_channels);
    m_follow->setObjectName(QStringLiteral("timingFollow"));
    toolbar->addWidget(m_follow);
    m_export->setObjectName(QStringLiteral("timingExport"));
    m_export->setAccessibleName(tr("导出时序数据"));
    m_export->setEnabled(false);
    fluentToolTip(m_export, tr("导出时序数据"));
    toolbar->addWidget(m_export);
    m_import->setObjectName(QStringLiteral("timingImport"));
    m_import->setAccessibleName(tr("导入时序数据"));
    m_import->setEnabled(false);
    fluentToolTip(m_import, tr("导入时序数据"));
    toolbar->addWidget(m_import);
    layout->addLayout(toolbar);
    m_plot->setObjectName(QStringLiteral("digitalTimingPlot"));
    m_plot->setAccessibleName(tr("数字时序图"));
    layout->addWidget(m_plot, 1);
    auto *plotLayout = new QVBoxLayout(m_plot);
    plotLayout->setContentsMargins(6, 0, 6, 1);
    plotLayout->addStretch();
    m_readout->setObjectName(QStringLiteral("timingReadout"));
    plotLayout->addWidget(m_readout);

    m_refreshTimer.setInterval(40);
    m_autoFlushTimer.setInterval(1000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &DigitalTimingChartWidget::refresh);
    connect(&m_autoFlushTimer, &QTimer::timeout, this, &DigitalTimingChartWidget::flushAutoExport);
    connect(m_window, &FluentQt::ComboBox::currentIndexChanged, this, [this] {
        const int seconds = m_window->currentData().toInt();
        if (seconds > 0) {
            setWindowSeconds(seconds);
            saveViewPreference(QStringLiteral("timingWindow"), seconds);
        } else {
            const QSignalBlocker blocker(m_window);
            m_window->setCurrentIndex(m_window->findData(m_windowSeconds));
            QMetaObject::invokeMethod(this, &DigitalTimingChartWidget::editWindow, Qt::QueuedConnection);
        }
    });
    connect(m_channels, &QPushButton::clicked, this, &DigitalTimingChartWidget::selectChannels);
    connect(m_follow, &QPushButton::clicked, this, [this] {
        if (m_plot->recording())
            returnToLive();
        else
            m_plot->setFollowing(!m_plot->following());
    });
    connect(m_export, &QToolButton::clicked, this, &DigitalTimingChartWidget::showExportMenu);
    connect(m_import, &QToolButton::clicked, this, &DigitalTimingChartWidget::openRecording);
    m_plot->viewChanged = [this] { updateReadout(); };
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this, [this] {
        if (m_plot->recording() || TimingSource::usesChannels(properties()))
            updateChannels();
    });
    updateReadout();
}

QJsonObject DigitalTimingChartWidget::bitProfile() const
{
    const auto name = text("bitProfile");
    return effectiveBitProfile(m_bitProfiles, name.isEmpty() ? QStringLiteral("default") : name);
}

void DigitalTimingChartWidget::setBitProfiles(const QJsonObject &profiles)
{
    m_bitProfiles = profiles;
    apply();
}

void DigitalTimingChartWidget::apply()
{
    const QString title = text("title").isEmpty() ? tr("数字时序图") : text("title");
    m_title->setText(title);
    m_title->setVisible(flag("showTitle"));
    setAccessibleName(title);
    m_readout->setVisible(flag("showValue"));
    m_plot->setValueTagsVisible(flag("showValue"));
    updateChannels();
    if (!isLive()) {
        m_plot->resetHistory();
        if (!TimingSource::usesChannels(properties())) {
            if (const auto sample = word(prop("value").toVariant()))
                m_plot->append({0, {*sample, quint16((1u << m_bitWidth) - 1)}});
        }
        m_plot->advance(m_windowSeconds);
    }
}

std::optional<quint16> DigitalTimingChartWidget::word(const QVariant &value) const
{
    const auto number = bindingFloat(value);
    if (value.typeId() == QMetaType::Bool || !number || *number < 0 || *number >= (1 << m_bitWidth) ||
        std::floor(*number) != *number)
        return {};
    return quint16(*number);
}

qreal DigitalTimingChartWidget::elapsed() const { return m_clock.isValid() ? m_clock.nsecsElapsed() / 1e9 : 0; }

void DigitalTimingChartWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    const auto sample = word(value);
    appendSample({elapsed(), sample ? TimingLevels{*sample, quint16((1u << m_bitWidth) - 1)} : TimingLevels{}});
}

void DigitalTimingChartWidget::bindingStatusChanged(BindingStatus status)
{
    if (status == BindingStatus::Stale && m_plot->hasLiveSamples())
        appendSample({elapsed(), {}});
}

void DigitalTimingChartWidget::onAttached()
{
    m_clock.start();
    m_plot->resetHistory();
    setWindowSeconds(viewPreference(QStringLiteral("timingWindow"), m_windowSeconds).toInt());
    m_exportDirectory = viewPreference(QStringLiteral("timingExportDirectory"), QString()).toString();
    m_autoExport = viewPreference(QStringLiteral("timingAutoExport"), false).toBool() && !m_exportDirectory.isEmpty();
    m_channelMask = quint16(viewPreference(channelPreference(), 0xffff).toUInt());
    if (TimingSource::usesChannels(properties())) {
        updateChannels();
        bindChannels();
    } else {
        setBitProfiles(context()->bitProfiles());
        live(connect(context(), &RuntimeContext::bitProfilesChanged, this,
                     [this] { setBitProfiles(context()->bitProfiles()); }));
    }
    live(connect(context(), &RuntimeContext::activeChanged, this, &DigitalTimingChartWidget::updateRuntimeState));
    updateRuntimeState();
}

void DigitalTimingChartWidget::updateRuntimeState()
{
    const bool active = context()->isActive();
    m_export->setEnabled(active && !m_plot->recording());
    m_import->setEnabled(active);
    if (active) {
        updatePointChannels(context()->projectData()->values());
        if (m_autoExport)
            startAutoExport();
    } else {
        stopAutoExport();
        if (m_plot->hasLiveSamples())
            appendSample({elapsed(), {}});
        for (auto &input : m_inputs)
            input.stream = 0;
    }
    updateRefreshTimer();
}

void DigitalTimingChartWidget::bindChannels()
{
    m_inputs.resize(chartCurveCount(properties()));
    for (int i = 0; i < m_inputs.size(); ++i) {
        auto entry = BindingEntry::fromWidget({{QStringLiteral("type"), typeName()},
                                              {QStringLiteral("binding"), chartCurveBinding(properties(), i)}});
        if (entry && context()->resolveBindingSource(*entry))
            m_inputs[i].binding = std::move(entry);
    }
    live(connect(context()->projectData(), &ProjectData::valuesChanged, this,
                 &DigitalTimingChartWidget::updatePointChannels));

    auto *hub = context()->hub();
    live(connect(hub, &CommHub::canFrameDecoded, this,
                 [this, hub](const CanFrame &frame, const Message &fields, const DataSource &source) {
        if (!context()->isActive() || frame.rtr || frame.error)
            return;
        TimingSample sample{elapsed(), m_plot->currentLevels()};
        const CanId id{frame.id, frame.extended};
        for (int i = 0; i < m_inputs.size(); ++i) {
            auto &input = m_inputs[i];
            if (!input.binding || !input.binding->route().accepts(source))
                continue;
            const auto &binding = *input.binding;
            QVariant raw;
            if (binding.parseMode == ParseMode::CanField) {
                raw = binding.extractFromParsed(fields);
                if (!raw.isValid()) {
                    // Missing fields end the trace only for a frame that defines this signal.
                    const auto *link = qobject_cast<CanLink *>(hub->sourceLink(source));
                    const auto set = link ? link->signalSet() : nullptr;
                    if (!set || !set->definesSignal(id, binding.key))
                        continue;
                }
            } else if (binding.parseMode == ParseMode::CanSignal && binding.canId.key() == id.key()) {
                raw = binding.extractFromCan(frame);
            } else {
                continue;
            }
            input.stream = source.stream;
            sample.levels.setLevel(i, digitalLevel(binding, raw));
        }
        appendSample(sample);
    }));
    live(connect(hub, &CommHub::streamClosed, this, [this](const DataSource &source) {
        if (!source.stream || !context()->isActive())
            return;
        TimingSample sample{elapsed(), m_plot->currentLevels()};
        for (int i = 0; i < m_inputs.size(); ++i) {
            if (m_inputs[i].stream == source.stream) {
                m_inputs[i].stream = 0;
                sample.levels.setLevel(i, {});
            }
        }
        appendSample(sample);
    }));
}

void DigitalTimingChartWidget::updatePointChannels(const Message &values)
{
    if (m_inputs.isEmpty() || !context()->isActive())
        return;
    TimingSample sample{elapsed(), m_plot->currentLevels()};
    for (int i = 0; i < m_inputs.size(); ++i) {
        const auto &binding = m_inputs.at(i).binding;
        if (binding && binding->parseMode == ParseMode::DataPoint && values.contains(binding->key))
            sample.levels.setLevel(i, digitalLevel(*binding, values.value(binding->key)));
    }
    appendSample(sample);
}

void DigitalTimingChartWidget::appendSample(const TimingSample &sample)
{
    const bool first = !m_plot->hasLiveSamples();
    if (first && !sample.levels.valid && TimingSource::usesChannels(properties()))
        return;
    if (!m_plot->append(sample))
        return;
    QString error;
    if (isLive() && !m_historyFailed && !m_history.append(sample, &error)) {
        m_historyFailed = true;
        context()->notify(tr("时序记录失败"), error, NotifyType::Error);
    }
    if (m_autoFile.isOpen())
        writeAutoExport(timingCsvRow(sample, m_autoColumns));
    if (first)
        updateRefreshTimer();
}

void DigitalTimingChartWidget::onDetached()
{
    stopAutoExport();
    m_refreshTimer.stop();
    m_export->setEnabled(false);
    m_import->setEnabled(false);
    m_inputs.clear();
    m_recordingName.clear();
    m_plot->setRecording(std::nullopt);
    m_plot->resetHistory();
    m_history.clear();
    m_historyFailed = false;
}

void DigitalTimingChartWidget::showEvent(QShowEvent *event)
{
    HmiWidget::showEvent(event);
    updateRefreshTimer();
}

void DigitalTimingChartWidget::hideEvent(QHideEvent *event)
{
    m_refreshTimer.stop();
    HmiWidget::hideEvent(event);
}

void DigitalTimingChartWidget::refresh() { m_plot->advance(elapsed()); }

void DigitalTimingChartWidget::updateRefreshTimer()
{
    if (context() && context()->isActive() && isVisible() && m_plot->hasLiveSamples() && !m_plot->recording()) {
        refresh();
        m_refreshTimer.start();
    } else {
        m_refreshTimer.stop();
    }
}

void DigitalTimingChartWidget::updateReadout()
{
    const auto selected = m_plot->displayedLevels();
    const auto inspection = m_plot->inspectionTime();
    const auto *recording = m_plot->recording();
    const bool channels = TimingSource::usesChannels(properties());
    QString status;
    if (recording) {
        status = tr("记录 · %1").arg(m_recordingName);
    } else if (channels && m_plot->hasLiveSamples()) {
        status = tr("有效 %1/%2 路").arg(qPopulationCount(selected.valid)).arg(chartCurveCount(properties()));
    } else if (selected.valid) {
        status = QStringLiteral("0x") +
                 QString::number(selected.high, 16).rightJustified(m_bitWidth / 4, QLatin1Char('0')).toUpper();
    } else if (inspection || !m_plot->following()) {
        status = QStringLiteral("—");
    } else if (bindingStatus() == BindingStatus::Stale) {
        status = tr("数据已过期");
    } else {
        status = m_plot->hasLiveSamples() ? tr("数据异常") : tr("待数据");
    }
    if (inspection)
        status = tr("游标 %1 s · %2").arg(QLocale().toString(*inspection, 'f', 3), status);
    if (m_autoFile.isOpen())
        status += QStringLiteral(" · ") + tr("自动导出中");
    m_readout->setText(status);
    setAccessibleDescription(recording || channels ? status + QStringLiteral(" · ") +
                                                         m_plot->describeChannels(selected).join(QStringLiteral(" · "))
                                                   : status);
    m_follow->setText(!recording && m_plot->following() ? tr("暂停") : tr("回到实时"));
}

void DigitalTimingChartWidget::updateChannels()
{
    const bool channels = TimingSource::usesChannels(properties());
    const auto definitions = channelDefinitions();
    m_channelNames.clear();
    for (const auto &definition : definitions)
        m_channelNames.append(definition.toObject().value(QStringLiteral("name")).toString());
    if (!channels)
        m_bitWidth = int(definitions.size());
    if (const auto *recording = m_plot->recording())
        m_plot->setChannels(namedChannels(recording->names), m_recordingMask, true);
    else
        m_plot->setChannels(definitions, m_channelMask, channels);
    updateReadout();
}

QJsonArray DigitalTimingChartWidget::channelDefinitions() const
{
    if (!TimingSource::usesChannels(properties()))
        return bitProfile().value(QStringLiteral("bits")).toArray();
    QStringList names;
    for (int i = 0; i < chartCurveCount(properties()); ++i)
        names.append(chartCurveName(properties(), i));
    return namedChannels(names);
}

QJsonArray DigitalTimingChartWidget::displayedChannels() const
{
    if (const auto *recording = m_plot->recording())
        return namedChannels(recording->names);
    return channelDefinitions();
}

QString DigitalTimingChartWidget::channelPreference() const
{
    return TimingSource::usesChannels(properties()) ? QStringLiteral("timingBoundChannels")
                                                   : QStringLiteral("timingChannels");
}

void DigitalTimingChartWidget::selectChannels()
{
    auto *menu = new FluentQt::CheckableMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const auto select = [this](quint16 mask) {
        if (m_plot->recording()) {
            m_recordingMask = mask;
        } else {
            m_channelMask = mask;
            saveViewPreference(channelPreference(), mask);
        }
        updateChannels();
    };
    connect(menu->addAction(tr("全部显示")), &QAction::triggered, this, [select] { select(0xffff); });
    menu->addSeparator();
    const auto bits = displayedChannels();
    const bool channels = m_plot->recording() || TimingSource::usesChannels(properties());
    const quint16 shown = m_plot->recording() ? m_recordingMask : m_channelMask;
    for (int i = 0; i < bits.size(); ++i) {
        const quint16 mask = quint16(1u << i);
        const QString name = bits.at(i).toObject().value(QStringLiteral("name")).toString();
        const QString bit = QStringLiteral("B%1").arg(i);
        auto *action = menu->addCheckableAction(channels || name == bit ? name : bit + QStringLiteral(" · ") + name,
                                                (shown & mask) != 0);
        connect(action, &QAction::triggered, this, [this, mask, select] {
            select((m_plot->recording() ? m_recordingMask : m_channelMask) ^ mask);
        });
    }
    menu->exec(m_channels->mapToGlobal(QPoint(0, m_channels->height())), true);
}

void DigitalTimingChartWidget::setWindowSeconds(int seconds)
{
    m_windowSeconds = std::clamp(seconds, 1, kHistorySeconds);
    const QSignalBlocker blocker(m_window);
    const QString label = tr("%1 秒").arg(m_windowSeconds);
    if (m_window->findData(m_windowSeconds) < 0) {
        if (m_customWindowIndex < 0) {
            m_customWindowIndex = m_window->findData(0);
            m_window->insertItem(m_customWindowIndex, label, {}, m_windowSeconds);
        } else {
            m_window->setItemText(m_customWindowIndex, label);
            m_window->setItemData(m_customWindowIndex, m_windowSeconds);
        }
    }
    m_window->setCurrentIndex(m_window->findData(m_windowSeconds));
    m_plot->setWindow(m_windowSeconds);
    updateReadout();
    if (!isLive())
        m_plot->advance(m_windowSeconds);
}

void DigitalTimingChartWidget::editWindow()
{
    AppDialog dialog(tr("自定义时间窗"), window());
    dialog.setPreferredSize(QSize(390, 205));
    auto *seconds = new FluentQt::SpinBox(&dialog);
    seconds->setObjectName(QStringLiteral("timingWindowSeconds"));
    seconds->setAccessibleName(tr("时间窗（秒）"));
    seconds->setRange(1, kHistorySeconds);
    seconds->setSuffix(tr(" 秒"));
    seconds->setValue(m_windowSeconds);
    dialog.viewLayout()->addWidget(seconds);
    if (dialog.exec() == QDialog::Accepted) {
        setWindowSeconds(seconds->value());
        saveViewPreference(QStringLiteral("timingWindow"), m_windowSeconds);
    }
}

bool DigitalTimingChartWidget::saveHistory(const QString &path, QString *error) const
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!m_history.write(&file, chartFileFormat(path), m_channelNames, elapsed(), error))
        return false;
    if (file.commit())
        return true;
    if (error)
        *error = file.errorString();
    return false;
}

bool DigitalTimingChartWidget::loadHistory(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    auto recording = readTimingRecording(file.readAll(), chartFileFormat(path), error);
    if (!recording)
        return false;
    m_recordingMask = 0xffff;
    m_recordingName = QFileInfo(path).fileName();
    m_plot->setRecording(std::move(recording));
    m_export->setEnabled(false);
    updateChannels();
    updateRefreshTimer();
    return true;
}

void DigitalTimingChartWidget::openRecording()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("导入时序数据"), {},
                                                       tr("时序文件 (*.csv *.json);;CSV文件 (*.csv);;JSON文件 (*.json)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!loadHistory(path, &error))
        context()->notify(tr("时序数据导入失败"), error, NotifyType::Error);
}

void DigitalTimingChartWidget::returnToLive()
{
    m_recordingName.clear();
    m_plot->advance(elapsed());
    m_plot->setRecording(std::nullopt);
    m_export->setEnabled(context() && context()->isActive());
    updateChannels();
    updateRefreshTimer();
}

void DigitalTimingChartWidget::showExportMenu()
{
    FluentQt::CheckableMenu menu(this);
    auto *save = menu.addAction(tr("导出完整时序数据…"));
    save->setEnabled(!m_history.isEmpty());
    auto *automatic = menu.addCheckableAction(tr("自动导出 CSV"), m_autoExport);
    auto *folder = menu.addAction(tr("打开导出目录"));
    folder->setEnabled(!m_exportDirectory.isEmpty());
    const auto *action = menu.execAndWait(m_export->mapToGlobal(QPoint(0, m_export->height())), true);
    if (action == save) {
        QString filter;
        QString path = QFileDialog::getSaveFileName(this, tr("导出时序数据"), QString(),
                                                    tr("CSV文件 (*.csv);;JSON文件 (*.json)"), &filter);
        if (path.isEmpty())
            return;
        if (QFileInfo(path).suffix().isEmpty())
            path += filter.contains(QLatin1String("*.json")) ? QStringLiteral(".json") : QStringLiteral(".csv");
        QString error;
        if (!saveHistory(path, &error))
            context()->notify(tr("时序数据导出失败"), error, NotifyType::Error);
    } else if (action == automatic) {
        if (m_autoExport) {
            m_autoExport = false;
            saveViewPreference(QStringLiteral("timingAutoExport"), false);
            stopAutoExport();
        } else {
            const QString path = QFileDialog::getExistingDirectory(this, tr("选择时序自动导出目录"), m_exportDirectory);
            if (path.isEmpty())
                return;
            m_exportDirectory = path;
            saveViewPreference(QStringLiteral("timingExportDirectory"), path);
            m_autoExport = true;
            startAutoExport();
            if (m_autoFile.isOpen())
                saveViewPreference(QStringLiteral("timingAutoExport"), true);
        }
    } else if (action == folder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_exportDirectory));
    }
}

void DigitalTimingChartWidget::startAutoExport()
{
    if (m_autoFile.isOpen())
        return;
    const QString suffix = QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString name = QStringLiteral("timing-%1-%2.csv")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")), suffix);
    m_autoFile.setFileName(QDir(m_exportDirectory).filePath(name));
    if (!m_autoFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        autoExportFailed(m_autoFile.errorString());
        return;
    }
    m_autoColumns = m_channelNames.size();
    writeAutoExport(timingCsvHeader(m_channelNames) + timingCsvRow({elapsed(), m_plot->currentLevels()}, m_autoColumns));
    if (m_autoFile.isOpen())
        m_autoFlushTimer.start();
    updateReadout();
}

void DigitalTimingChartWidget::stopAutoExport()
{
    if (!m_autoFile.isOpen())
        return;
    m_autoFlushTimer.stop();
    writeAutoExport(timingCsvRow({elapsed(), m_plot->currentLevels()}, m_autoColumns));
    flushAutoExport();
    m_autoFile.close();
    updateReadout();
}

void DigitalTimingChartWidget::writeAutoExport(const QByteArray &bytes)
{
    if (m_autoFile.write(bytes) != bytes.size())
        autoExportFailed(m_autoFile.errorString());
}

void DigitalTimingChartWidget::flushAutoExport()
{
    if (m_autoFile.isOpen() && !m_autoFile.flush())
        autoExportFailed(m_autoFile.errorString());
}

void DigitalTimingChartWidget::autoExportFailed(const QString &error)
{
    m_autoFlushTimer.stop();
    m_autoFile.close();
    m_autoExport = false;
    saveViewPreference(QStringLiteral("timingAutoExport"), false);
    context()->notify(tr("时序自动导出已停止"), error, NotifyType::Error);
    updateReadout();
}

} // namespace fhmi
