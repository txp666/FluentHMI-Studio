#include "runtime/hmi_widget.h"

#include "runtime/runtime_context.h"
#include "runtime/widgets/basic_widgets.h"
#include "runtime/widgets/container_widgets.h"
#include "runtime/widgets/data_widgets.h"
#include "runtime/widgets/display_widgets.h"
#include "runtime/widgets/link_cards.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Label.h>

#include <QtCore/QHash>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>

namespace fhmi {

HmiWidget::HmiWidget(const WidgetMeta &meta, QWidget *parent) : QWidget(parent), m_meta(&meta)
{
    setAttribute(Qt::WA_StyledBackground, false);
}

void HmiWidget::setProperties(const QJsonObject &props)
{
    m_props = normalizedRangeProperties(typeName(), props);
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

void HmiWidget::attach(RuntimeContext *context)
{
    if (m_context == context) {
        return;
    }
    detach();
    m_context = context;
    if (context) {
        onAttached();
    }
}

void HmiWidget::detach()
{
    if (m_context) {
        onDetached();
        for (const QMetaObject::Connection &connection : std::as_const(m_liveConnections)) {
            QObject::disconnect(connection);
        }
        m_liveConnections.clear();
        m_context = nullptr;
    }
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
    void apply() override
    {
        m_label->setText(tr("%1\nC++ 运行时暂未实现").arg(meta().displayName));
    }

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
        {QStringLiteral("DataTable"), make<DataTableWidget>()},
        {QStringLiteral("LogBox"), make<LogBoxWidget>()},
        {QStringLiteral("DataRecvCard"), make<DataRecvCardWidget>()},
        {QStringLiteral("DataSendCard"), make<DataSendCardWidget>()},
        {QStringLiteral("SerialConfigCard"), make<SerialConfigCardWidget>()},
        {QStringLiteral("TcpConfigCard"), make<TcpConfigCardWidget>()},
        {QStringLiteral("MqttConfigCard"), make<MqttConfigCardWidget>()},
        {QStringLiteral("ModbusConfigCard"), make<ModbusConfigCardWidget>()},
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
    const Factory factory = factories().value(typeName);
    HmiWidget *widget = factory ? factory(*meta, parent) : new PlaceholderWidget(*meta, parent);
    widget->setObjectName(typeName);
    widget->blockSignals(true);
    if (!assetBaseDir.isEmpty()) {
        widget->setAssetBaseDir(assetBaseDir);
    }
    QJsonObject merged = meta->defaultProperties();
    for (auto it = props.begin(); it != props.end(); ++it) {
        merged.insert(it.key(), it.value());
    }
    widget->setProperties(merged);
    widget->blockSignals(false);
    widget->resize(meta->defaultWidth, meta->defaultHeight);
    return widget;
}

QIcon widgetIcon(const QString &iconName)
{
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
    return names.isEmpty() ? QStringList{QStringLiteral("标签1")} : names;
}

int clampTabIndex(int index, int count)
{
    return std::clamp(index, 0, std::max(0, count - 1));
}

} // namespace fhmi
