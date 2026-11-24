#include "runtime/hmi_widget.h"
#include "runtime/periodic_sender.h"
#include "runtime/widgets/alarm_widgets.h"
#include "runtime/widgets/s7_config_card.h"

#include "core/i18n.h"
#include "core/industrial_catalog.h"
#include "runtime/widgets/industrial_widget.h"

#include "runtime/runtime_context.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/basic_widgets.h"
#include "runtime/widgets/can_widgets.h"
#include "runtime/widgets/container_widgets.h"
#include "runtime/widgets/data_widgets.h"
#include "runtime/widgets/digital_timing_widget.h"
#include "runtime/widgets/display_widgets.h"
#include "runtime/widgets/link_cards.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/InfoBadge.h>
#include <FluentQtWidgets/Widgets/Label.h>

#include <QtCore/QHash>
#include <QtCore/QSettings>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace fhmi {

HmiWidget::HmiWidget(const WidgetMeta &meta, QWidget *parent) : QWidget(parent), m_meta(&meta)
{
    setAttribute(Qt::WA_StyledBackground, false);
}

void HmiWidget::setProperties(const QJsonObject &props)
{
    stopPeriodicSend();
    m_props = normalizedWidgetProperties(typeName(), props);
    apply();
}

void HmiWidget::setAssetBaseDir(const QString &dir)
{
    if (m_assetBaseDir == dir) {
        return;
    }
    m_assetBaseDir = dir;
    apply();
}

void HmiWidget::applyBoundValue(const QVariant &, const BindingEntry &) {}

void HmiWidget::setBindingStatus(BindingStatus status)
{
    if (m_bindingStatus != status) {
        m_bindingStatus = status;
        bindingStatusChanged(status);
    }
}

void HmiWidget::setBindingProblem(const QString &status, const QString &detail)
{
    if (!m_bindingBadge && status.isEmpty())
        return;
    if (!m_bindingBadge) {
        m_bindingBadge = new FluentQt::InfoBadge(this);
        m_bindingBadge->setObjectName(QStringLiteral("bindingQualityBadge"));
        m_bindingBadge->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        m_bindingBadge->setMargin(3);
        m_bindingBadge->setCustomBackgroundColor(QColor(0x85402B), QColor(0x85402B));
        fluentToolTip(m_bindingBadge);
    }
    const QString description = detail.isEmpty() ? status : status + QStringLiteral(" · ") + detail;
    m_bindingBadge->setText(status);
    m_bindingBadge->setToolTip(description);
    m_bindingBadge->setAccessibleDescription(description);
    m_bindingBadge->setVisible(!status.isEmpty());
    if (!status.isEmpty()) {
        positionBindingBadge();
        m_bindingBadge->raise();
    }
}

void HmiWidget::positionBindingBadge()
{
    if (m_bindingBadge && !m_bindingBadge->isHidden()) {
        m_bindingBadge->adjustSize();
        m_bindingBadge->move(std::max(1, width() - m_bindingBadge->width() - 2), 1);
    }
}

void HmiWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionBindingBadge();
}

int HmiWidget::contentMinimumHeight(int width)
{
    arrangeForWidth(width);
    layout()->invalidate();
    layout()->activate();
    return minimumSizeHint().height();
}

QSize HmiWidget::sizeForGeometry(const QSize &requested)
{
    if (!fitsContentHeight()) {
        return requested;
    }
    const int minimum = contentMinimumHeight(requested.width());
    if (maximumHeight() == QWIDGETSIZE_MAX) {
        setMinimumHeight(minimum);
    }
    return QSize(requested.width(), std::max(requested.height(), minimum));
}

void HmiWidget::updateMinimumHeight()
{
    const bool fitted = minimumHeight() > 0 && height() == minimumHeight();
    const int minimum = contentMinimumHeight(width());
    // The design canvas owns fixed geometry; its next sizeForGeometry() call applies the new minimum.
    if (maximumHeight() != QWIDGETSIZE_MAX) {
        return;
    }
    setMinimumHeight(minimum);
    if (fitted) {
        resize(width(), minimum);
    }
}

void HmiWidget::attach(RuntimeContext *context)
{
    if (m_context == context) {
        return;
    }
    detach();
    m_context = context;
    if (context) {
        if (meta().prop(CommandEncoder::SendInterval)) {
            const auto id = property("fhmiWidgetId").toString();
            auto *sender = context->periodicSender();
            live(connect(sender, &PeriodicSender::changed, this, [this, id](const QString &changed) {
                if (changed == id)
                    periodicSendChanged(isPeriodicSending());
            }));
            live(connect(this, &QObject::destroyed, sender, [sender, id] { sender->stop(id); }));
        }
        onAttached();
    }
}

void HmiWidget::detach()
{
    stopPeriodicSend();
    if (m_context) {
        onDetached();
        for (const QMetaObject::Connection &connection : std::as_const(m_liveConnections)) {
            QObject::disconnect(connection);
        }
        m_liveConnections.clear();
        m_context = nullptr;
    }
}

QVariant HmiWidget::viewPreference(const QString &key, const QVariant &fallback) const
{
    const QString group = m_context ? m_context->widgetPreferencesGroup(*this) : QString();
    return group.isEmpty() ? fallback : QSettings().value(group + u'/' + key, fallback);
}

void HmiWidget::saveViewPreference(const QString &key, const QVariant &value)
{
    const QString group = m_context ? m_context->widgetPreferencesGroup(*this) : QString();
    if (!group.isEmpty())
        QSettings().setValue(group + u'/' + key, value);
}

void HmiWidget::rememberViewOption(FluentQt::ComboBox *control, const QString &key)
{
    const int index = control->findData(viewPreference(key, control->currentData()));
    if (index >= 0)
        control->setCurrentIndex(index);
    live(connect(control, &FluentQt::ComboBox::currentIndexChanged, this, [this, control, key] {
        saveViewPreference(key, control->currentData());
    }));
}

void HmiWidget::rememberViewOption(QAbstractButton *control, const QString &key)
{
    control->setChecked(viewPreference(key, control->isChecked()).toBool());
    live(connect(control, &QAbstractButton::toggled, this, [this, key](bool checked) {
        saveViewPreference(key, checked);
    }));
}

void HmiWidget::rememberViewOption(QSpinBox *control, const QString &key)
{
    control->setValue(viewPreference(key, control->value()).toInt());
    live(connect(control, &QSpinBox::valueChanged, this, [this, key](int value) {
        saveViewPreference(key, value);
    }));
}

void HmiWidget::requestSend(const CommandResult &command, std::function<void()> onSent, const QString &mode,
                            int intervalMs, CommandReader readInputs)
{
    if (m_context) {
        m_context->requestSend(this, command, confirmsSend(), flag("showSendConfirmationDetails"),
                               mode.isEmpty() ? text("commandMode") : mode, std::move(onSent), intervalMs,
                               std::move(readInputs));
    }
}

bool HmiWidget::isPeriodicSending() const
{
    return m_context && m_context->periodicSender()->running(property("fhmiWidgetId").toString());
}

bool HmiWidget::isPeriodicStarting() const
{
    return m_context && m_context->periodicSender()->starting(property("fhmiWidgetId").toString());
}

bool HmiWidget::stopPeriodicSend()
{
    return m_context && m_context->periodicSender()->stop(property("fhmiWidgetId").toString());
}

const QJsonObject &HmiWidget::frameTemplates() const
{
    static const QJsonObject empty;
    return m_context ? m_context->frameTemplates() : empty;
}

QWidget *HmiWidget::childHost(int)
{
    return nullptr;
}

QJsonValue HmiWidget::prop(const char *name) const
{
    const QString key = QLatin1String(name);
    const QJsonValue value = m_props.value(key);
    if (!value.isUndefined() && !value.isNull()) {
        return value;
    }
    const PropDef *def = m_meta->prop(key);
    return def ? def->defaultValue : QJsonValue();
}

QString HmiWidget::text(const char *name) const
{
    const QJsonValue value = prop(name);
    return value.isString() ? value.toString() : value.toVariant().toString();
}

int HmiWidget::integer(const char *name) const
{
    const QJsonValue value = prop(name);
    if (value.isString()) {
        return value.toString().toInt();
    }
    return int(value.toDouble());
}

double HmiWidget::real(const char *name) const
{
    const QJsonValue value = prop(name);
    return value.isString() ? value.toString().toDouble() : value.toDouble();
}

bool HmiWidget::flag(const char *name) const
{
    const QJsonValue value = prop(name);
    return value.isBool() ? value.toBool() : value.toDouble() != 0;
}

// ---------------------------------------------------------------------------
// Placeholder and factory
// ---------------------------------------------------------------------------

namespace {

class PlaceholderWidget : public HmiWidget
{
  public:
    explicit PlaceholderWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8);
        m_label = new FluentQt::CaptionLabel(this);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setWordWrap(true);
        layout->addWidget(m_label);
        setStyleSheet(QStringLiteral("PlaceholderWidget { border: 1px dashed rgba(128,128,128,0.5); "
                                     "border-radius: 6px; }"));
        setAttribute(Qt::WA_StyledBackground, true);
    }

  protected:
    void apply() override { m_label->setText(tr("%1\nC++ 运行时暂未实现").arg(translated(meta().displayName))); }

  private:
    FluentQt::CaptionLabel *m_label = nullptr;
};

using Factory = std::function<HmiWidget *(const WidgetMeta &, QWidget *)>;

template <typename T> Factory make()
{
    return [](const WidgetMeta &meta, QWidget *parent) -> HmiWidget * { return new T(meta, parent); };
}

const QHash<QString, Factory> &factories()
{
    static const QHash<QString, Factory> table = {
        {QStringLiteral("PushButton"), make<PushButtonWidget>()},
        {QStringLiteral("Label"), make<LabelWidget>()},
        {QStringLiteral("LineEdit"), make<LineEditWidget>()},
        {QStringLiteral("ComboBox"), make<ComboBoxWidget>()},
        {QStringLiteral("ToggleSwitch"), make<ToggleSwitchWidget>()},
        {QStringLiteral("Slider"), make<SliderWidget>()},
        {QStringLiteral("SpinBox"), make<SpinBoxWidget>()},
        {QStringLiteral("LEDIndicator"), make<LedWidget>()},
        {QStringLiteral("AlarmBanner"), make<AlarmBannerWidget>()},
        {QStringLiteral("AlarmList"), make<AlarmListWidget>()},
        {QStringLiteral("NumericDisplay"), make<NumericDisplayWidget>()},
        {QStringLiteral("ProgressBar"), make<ProgressBarWidget>()},
        {QStringLiteral("ProgressRingDisplay"), make<ProgressRingWidget>()},
        {QStringLiteral("ImageDisplay"), make<ImageWidget>()},
        {QStringLiteral("GaugeMeter"), make<GaugeWidget>()},
        {QStringLiteral("HBoxLayout"), make<BoxContainerWidget>()},
        {QStringLiteral("VBoxLayout"), make<BoxContainerWidget>()},
        {QStringLiteral("FlexLayout"), make<BoxContainerWidget>()},
        {QStringLiteral("GroupBox"), make<GroupBoxWidget>()},
        {QStringLiteral("CardContainer"), make<CardContainerWidget>()},
        {QStringLiteral("TabWidget"), make<TabContainerWidget>()},
        {QStringLiteral("Separator"), make<SeparatorWidget>()},
        {QStringLiteral("RealtimeChart"), make<RealtimeChartWidget>()},
        {QStringLiteral("DigitalTimingChart"), make<DigitalTimingChartWidget>()},
        {QStringLiteral("DataTable"), make<DataTableWidget>()},
        {QStringLiteral("LogBox"), make<LogBoxWidget>()},
        {QStringLiteral("ModbusSlaveTable"), make<ModbusSlaveTableWidget>()},
        {QStringLiteral("DataRecvCard"), make<DataRecvCardWidget>()},
        {QStringLiteral("DataSendCard"), make<DataSendCardWidget>()},
        {QStringLiteral("S7ConfigCard"), make<S7ConfigCardWidget>()},
        {QStringLiteral("SerialConfigCard"), make<SerialConfigCardWidget>()},
        {QStringLiteral("TcpConfigCard"), make<TcpConfigCardWidget>()},
        {QStringLiteral("MqttConfigCard"), make<MqttConfigCardWidget>()},
        {QStringLiteral("UdpConfigCard"), make<UdpConfigCardWidget>()},
        {QStringLiteral("CanConfigCard"), make<CanConfigCardWidget>()},
        {QStringLiteral("CanMonitor"), make<CanMonitorWidget>()},
    };
    return table;
}

} // namespace

HmiWidget *createHmiWidget(const QString &typeName, const QJsonObject &props, QWidget *parent,
                           const QString &assetBaseDir)
{
    const WidgetMeta *meta = WidgetRegistry::instance().find(typeName);
    if (!meta) {
        return nullptr;
    }
    const Factory fallback = isIndustrialCategory(meta->category) ? make<IndustrialWidget>() : make<PlaceholderWidget>();
    HmiWidget *widget = factories().value(typeName, fallback)(*meta, parent);
    widget->setObjectName(typeName);
    widget->blockSignals(true);
    if (!assetBaseDir.isEmpty()) {
        widget->setAssetBaseDir(assetBaseDir);
    }
    QJsonObject merged = meta->defaultProperties();
    const auto saved = normalizedWidgetProperties(typeName, props);
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        merged.insert(it.key(), it.value());
    }
    widget->setProperties(merged);
    widget->blockSignals(false);
    widget->resize(widget->sizeForGeometry(
        QSize(meta->defaultWidth, widget->fitsContentHeight() ? meta->minHeight : meta->defaultHeight)));
    return widget;
}

QIcon widgetIcon(const QString &iconName)
{
    // Pictures of controls that FluentQtWidgets has no icon for, and of the designer's side panels shown or hidden,
    // drawn in its style, one file per theme in resources/widget_icons. PyQt UI Designer has the same files under
    // the same names.
    static const QHash<QString, QString> own = {
        {QStringLiteral("button"), QStringLiteral("Button")},
        {QStringLiteral("text_field"), QStringLiteral("TextField")},
        {QStringLiteral("dropdown"), QStringLiteral("Dropdown")},
        {QStringLiteral("toggle"), QStringLiteral("Toggle")},
        {QStringLiteral("slider"), QStringLiteral("Slider")},
        {QStringLiteral("spin_box"), QStringLiteral("SpinBox")},
        {QStringLiteral("columns"), QStringLiteral("Columns")},
        {QStringLiteral("rows"), QStringLiteral("Rows")},
        {QStringLiteral("group_box"), QStringLiteral("GroupBox")},
        {QStringLiteral("tabs"), QStringLiteral("Tabs")},
        {QStringLiteral("card"), QStringLiteral("Card")},
        {QStringLiteral("led"), QStringLiteral("Led")},
        {QStringLiteral("seven_segment"), QStringLiteral("SevenSegment")},
        {QStringLiteral("progress_bar"), QStringLiteral("ProgressBar")},
        {QStringLiteral("progress_ring"), QStringLiteral("ProgressRing")},
        {QStringLiteral("table"), QStringLiteral("Table")},
        {QStringLiteral("serial_port"), QStringLiteral("SerialPort")},
        {QStringLiteral("panel_left"), QStringLiteral("PanelLeft")},
        {QStringLiteral("panel_left_on"), QStringLiteral("PanelLeftOn")},
        {QStringLiteral("panel_right"), QStringLiteral("PanelRight")},
        {QStringLiteral("panel_right_on"), QStringLiteral("PanelRightOn")},
    };
    const auto file = own.constFind(iconName);
    if (file != own.constEnd()) {
        const QString path = QStringLiteral(":/fluenthmi/widget_icons/%1_%2.svg");
        return FluentQt::themedIcon(path.arg(*file, QStringLiteral("black")), path.arg(*file, QStringLiteral("white")));
    }
    static const QHash<QString, FluentQt::FluentIcon> icons = [] {
        QHash<QString, FluentQt::FluentIcon> table;
        for (int i = 0; i <= int(FluentQt::FluentIcon::ExpressiveInputEntry); ++i) {
            const auto icon = FluentQt::FluentIcon(i);
            table.insert(FluentQt::iconName(icon), icon);
        }
        return table;
    }();
    const auto it = icons.constFind(iconName);
    return FluentQt::icon(it != icons.constEnd() ? *it : FluentQt::FluentIcon::Application);
}

QStringList tabNames(const QString &text)
{
    QStringList names;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) {
            names.append(line.trimmed());
        }
    }
    return names.isEmpty() ? QStringList{QStringLiteral("标签1")} : names; // i18n: value
}

int clampTabIndex(int index, int count)
{
    return std::clamp(index, 0, std::max(0, count - 1));
}

} // namespace fhmi
