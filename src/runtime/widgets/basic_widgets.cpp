#include "runtime/widgets/basic_widgets.h"

#include "runtime/runtime_context.h"

#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/Slider.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>

#include <QtCore/QLocale>
#include <QtCore/QSignalBlocker>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <cmath>

namespace fhmi {

namespace {

QHBoxLayout *fill(QWidget *owner, QWidget *child, const QMargins &margins = {})
{
    auto *layout = new QHBoxLayout(owner);
    layout->setContentsMargins(margins);
    layout->setSpacing(0);
    layout->addWidget(child);
    return layout;
}

QStringList nonEmptyLines(const QString &text)
{
    QStringList out;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) {
            out.append(line.trimmed());
        }
    }
    return out;
}

Qt::Alignment alignment(const QString &name)
{
    if (name == QLatin1String("Center")) {
        return Qt::AlignCenter;
    }
    if (name == QLatin1String("Right")) {
        return Qt::AlignRight | Qt::AlignVCenter;
    }
    return Qt::AlignLeft | Qt::AlignVCenter;
}

} // namespace

// ---------------------------------------------------------------------------
// PushButton
// ---------------------------------------------------------------------------

PushButtonWidget::PushButtonWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setButtonStyle(QStringLiteral("标准")); // i18n: value
}

void PushButtonWidget::setButtonStyle(const QString &style)
{
    QString resolved = style;
    if (style != QStringLiteral("强调") && style != QStringLiteral("轻量") && // i18n: value
        style != QStringLiteral("描边")) {                                    // i18n: value
        resolved = QStringLiteral("标准");                                    // i18n: value
    }
    if (resolved == m_buttonStyle) {
        return;
    }

    FluentQt::PushButton *button = nullptr;
    if (resolved == QStringLiteral("强调")) { // i18n: value
        button = new FluentQt::PrimaryPushButton(this);
    } else if (resolved == QStringLiteral("轻量")) { // i18n: value
        button = new FluentQt::TransparentPushButton(this);
    } else if (resolved == QStringLiteral("描边")) { // i18n: value
        button = new FluentQt::OutlinedPushButton(this);
        // OutlinedPushButton is checkable in the component library; this stays a momentary command button.
        button->setCheckable(false);
    } else {
        button = new FluentQt::PushButton(this);
    }
    if (m_button) {
        delete layout()->replaceWidget(m_button, button);
        delete m_button;
    } else {
        static_cast<QHBoxLayout *>(layout())->addWidget(button);
    }
    m_button = button;
    m_buttonStyle = resolved;
    m_button->show();
    // The connection belongs to the visual button. Replacing a style cannot duplicate a live command route.
    connect(m_button, &QPushButton::clicked, this, &PushButtonWidget::sendCommand);
}

void PushButtonWidget::apply()
{
    setButtonStyle(text("buttonStyle"));
    m_button->setText(text("text"));
    m_button->setEnabled(flag("enabled"));
}

void PushButtonWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    const QSignalBlocker blocker(m_button);
    m_button->setText(displayString(value));
}

void PushButtonWidget::sendCommand()
{
    if (!context()) {
        return;
    }
    QJsonObject props = properties();
    if (text("commandMode") == QLatin1String("S7")) {
        context()->send(CommandEncoder::s7FromProperties(props));
    } else if (text("commandMode") == QLatin1String("Modbus")) {
        // A button reads by default: its function code falls back to FC03.
        if (!props.contains(QStringLiteral("mbFunc"))) {
            props.insert(QStringLiteral("mbFunc"), QStringLiteral("03 读保持寄存器")); // i18n: value
        }
        if (CommandEncoder::functionCode(text("mbFunc")) == 0x05 && text("mbValue").toLongLong() == 1) {
            props.insert(QStringLiteral("mbValue"), 0xFF00);
        }
        context()->send(CommandEncoder::modbusFromProperties(props));
    } else if (const QString command = text("command").trimmed(); !command.isEmpty()) {
        context()->send(CommandEncoder::encode(props, command));
    }
    if (flag("notify")) {
        context()->notify(text("notifyTitle"), text("notifyContent"), text("notifyType"), integer("notifyDuration"));
    }
}

// ---------------------------------------------------------------------------
// Label
// ---------------------------------------------------------------------------

LabelWidget::LabelWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_label = new FluentQt::BodyLabel(this);
    fill(this, m_label);
}

void LabelWidget::apply()
{
    m_label->setText(text("text"));
    QFont font = m_label->font();
    font.setPointSize(integer("fontSize"));
    font.setBold(flag("bold"));
    m_label->setFont(font);
    m_label->setAlignment(alignment(text("alignment")));
}

void LabelWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    m_label->setText(displayString(value));
}

// ---------------------------------------------------------------------------
// LineEdit
// ---------------------------------------------------------------------------

LineEditWidget::LineEditWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_edit = new FluentQt::LineEdit(this);
    fill(this, m_edit);
}

void LineEditWidget::apply()
{
    m_edit->setPlaceholderText(text("placeholderText"));
    m_edit->setText(text("text"));
    m_edit->setReadOnly(flag("readOnly"));
}

void LineEditWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if(m_edit->hasFocus() && !m_edit->isReadOnly())return;
    const QSignalBlocker blocker(m_edit);
    m_edit->setText(displayString(value));
}

void LineEditWidget::onAttached()
{
    const bool modbus = text("commandMode") == QLatin1String("Modbus") || text("commandMode") == QLatin1String("S7");
    if (!flag("sendOnEnter") && !modbus) {
        return;
    }
    live(connect(m_edit, &QLineEdit::returnPressed, this, [this, modbus]() {
        const QString value = m_edit->text().trimmed();
        if (!value.isEmpty() || modbus) {
            context()->send(CommandEncoder::encode(properties(), value));
        }
    }));
}

// ---------------------------------------------------------------------------
// ComboBox
// ---------------------------------------------------------------------------

ComboBoxWidget::ComboBoxWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    rebuildSelection(QStringLiteral("下拉框"), {}); // i18n: value
}

void ComboBoxWidget::rebuildSelection(const QString &style, const QStringList &items)
{
    QWidget *previous = m_selection;
    m_combo = nullptr;
    m_segmented = nullptr;
    m_radioGroup = nullptr;
    m_selectionScroll = nullptr;
    m_selectionStyle = style;
    m_items = items;
    if (style == QStringLiteral("分段") || style == QStringLiteral("单选组")) { // i18n: value
        m_selectionScroll = new FluentQt::SingleDirectionScrollArea(Qt::Horizontal, this);
        m_selectionScroll->setObjectName(QStringLiteral("hmiOptionScroll"));
        m_selectionScroll->enableTransparentBackground();
        m_selectionScroll->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Keep the saved width and height. Fluent's overlay bar does not take space from a 36 px row.
        m_selectionScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    }
    if (style == QStringLiteral("分段")) { // i18n: value
        m_segmented = new FluentQt::SegmentedWidget(m_selectionScroll);
        m_segmented->setObjectName(QStringLiteral("hmiOptionSegments"));
        for (int i = 0; i < items.size(); ++i)
            m_segmented->addItem(QString::number(i), items.at(i));
        m_segmented->ensurePolished();
        m_segmented->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        connect(m_segmented, &FluentQt::SegmentedWidget::currentItemChanged, this,
                [this](const QString &key) { selectIndex(key.toInt()); });
        m_selectionScroll->setWidget(m_segmented);
    } else if (style == QStringLiteral("单选组")) { // i18n: value
        auto *content = new QWidget(m_selectionScroll);
        content->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        auto *row = new QHBoxLayout(content);
        row->setSizeConstraint(QLayout::SetMinimumSize);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(12);
        m_radioGroup = new QButtonGroup(content);
        m_radioGroup->setObjectName(QStringLiteral("hmiOptionGroup"));
        m_radioGroup->setExclusive(true);
        for (int i = 0; i < items.size(); ++i) {
            auto *radio = new FluentQt::RadioButton(items.at(i), content);
            radio->setObjectName(QStringLiteral("hmiOption%1").arg(i));
            radio->ensurePolished();
            radio->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            m_radioGroup->addButton(radio, i);
            row->addWidget(radio);
        }
        row->addStretch();
        connect(m_radioGroup, &QButtonGroup::idToggled, this, [this](int index, bool checked) {
            if (checked)
                selectIndex(index);
        });
        m_selectionScroll->setWidget(content);
    } else {
        m_combo = new FluentQt::ComboBox(this);
        m_combo->setObjectName(QStringLiteral("hmiOptionCombo"));
        m_combo->addItems(items);
        connect(m_combo, &FluentQt::ComboBox::currentIndexChanged, this, &ComboBoxWidget::selectIndex);
        m_selection = m_combo;
    }
    if (m_selectionScroll) {
        m_selection = m_selectionScroll;
        connect(m_selectionScroll->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this]() {
            // The range may change before Qt finishes positioning the content on first show or resize.
            QMetaObject::invokeMethod(this, [this]() { ensureSelectionVisible(); }, Qt::QueuedConnection);
        });
    }
    if (previous) {
        delete layout()->replaceWidget(previous, m_selection);
        delete previous;
    } else {
        static_cast<QHBoxLayout *>(layout())->addWidget(m_selection);
    }
    m_selection->show();
}

void ComboBoxWidget::apply()
{
    const QStringList items = nonEmptyLines(text("items"));
    QString style = text("selectionStyle");
    if (style != QStringLiteral("分段") && style != QStringLiteral("单选组")) // i18n: value
        style = QStringLiteral("下拉框");                                     // i18n: value
    const int configured = integer("currentIndex");
    const bool reset = !isLive() || !m_applied || configured != m_configuredIndex || items != m_items;
    int index = reset ? configured : m_currentIndex;
    if (index >= items.size() || (reset && index < 0))
        index = items.isEmpty() ? -1 : 0;
    if (style != m_selectionStyle || items != m_items)
        rebuildSelection(style, items);
    m_configuredIndex = configured;
    m_applied = true;
    setCurrentIndex(index);
}

void ComboBoxWidget::setCurrentIndex(int index)
{
    m_currentIndex = index;
    if (m_combo) {
        const QSignalBlocker blocker(m_combo);
        m_combo->setCurrentIndex(index);
    } else if (m_segmented) {
        if (index < 0) {
            // Pivot has no clear-current API. A fresh view represents the same empty selection as a combo.
            if (!m_segmented->currentItem().isEmpty())
                rebuildSelection(m_selectionStyle, m_items);
        } else {
            const QSignalBlocker blocker(m_segmented);
            m_segmented->setCurrentItem(QString::number(index));
        }
    } else {
        const QSignalBlocker groupBlocker(m_radioGroup);
        m_radioGroup->setExclusive(false);
        for (auto *radio : m_radioGroup->buttons()) {
            const QSignalBlocker blocker(radio);
            radio->setChecked(m_radioGroup->id(radio) == index);
        }
        m_radioGroup->setExclusive(true);
    }
    ensureSelectionVisible();
}

void ComboBoxWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto index = bindingInteger(value))
        setCurrentIndex(*index >= 0 && *index < m_items.size() ? *index : -1);
}

void ComboBoxWidget::selectIndex(int index)
{
    if (index < 0) {
        m_currentIndex = -1;
        return;
    }
    if (index >= m_items.size() || index == m_currentIndex)
        return;
    m_currentIndex = index;
    ensureSelectionVisible();
    if (!context() || (!flag("sendOnSelect") && text("commandMode") != QLatin1String("Modbus") &&
                       text("commandMode") != QLatin1String("S7")))
        return;
    // All selection views use the same command table; an empty command falls back to the option text.
    const QStringList commands = text("commands").split(QLatin1Char('\n'));
    const QString command = commands.value(index).trimmed();
    const QString raw = command.isEmpty() ? m_items.at(index) : command;
    if (!raw.isEmpty())
        context()->send(CommandEncoder::encode(properties(), raw));
}

void ComboBoxWidget::ensureSelectionVisible()
{
    if (!m_selectionScroll || m_currentIndex < 0)
        return;
    QWidget *selected = m_segmented ? static_cast<QWidget *>(m_segmented->item(QString::number(m_currentIndex)))
                                    : m_radioGroup->button(m_currentIndex);
    if (!selected)
        return;
    m_selectionScroll->ensureWidgetVisible(selected, 8, 0);
    // If one label itself is wider than the viewport, keep its leading indicator reachable.
    if (selected->width() + 16 > m_selectionScroll->viewport()->width())
        m_selectionScroll->horizontalScrollBar()->setValue(selected->x());
}

// ---------------------------------------------------------------------------
// ToggleSwitch
// ---------------------------------------------------------------------------

ToggleSwitchWidget::ToggleSwitchWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(0);
    setSwitchStyle(QStringLiteral("开关")); // i18n: value
}

void ToggleSwitchWidget::setSwitchStyle(const QString &style)
{
    const QString resolved = style == QStringLiteral("复选框") ? style : QStringLiteral("开关"); // i18n: value
    if (resolved == m_switchStyle)
        return;
    QWidget *previous = m_control;
    m_label = nullptr;
    m_switch = nullptr;
    m_checkBox = nullptr;
    m_switchStyle = resolved;
    if (resolved == QStringLiteral("复选框")) { // i18n: value
        m_checkBox = new FluentQt::CheckBox(this);
        m_checkBox->setObjectName(QStringLiteral("hmiCheckBox"));
        m_checkBox->setTristate(false);
        connect(m_checkBox, &QCheckBox::toggled, this, &ToggleSwitchWidget::checkedEdited);
        m_control = m_checkBox;
    } else {
        m_control = new QWidget(this);
        auto *row = new QHBoxLayout(m_control);
        row->setContentsMargins(0, 0, 0, 0);
        m_label = new FluentQt::BodyLabel(m_control);
        m_switch = new FluentQt::SwitchButton(m_control);
        m_switch->setObjectName(QStringLiteral("hmiToggleSwitch"));
        row->addWidget(m_label);
        row->addStretch();
        row->addWidget(m_switch);
        connect(m_switch, &FluentQt::SwitchButton::checkedChanged, this, &ToggleSwitchWidget::checkedEdited);
    }
    if (previous) {
        delete layout()->replaceWidget(previous, m_control);
        delete previous;
    } else {
        static_cast<QHBoxLayout *>(layout())->addWidget(m_control);
    }
    m_control->show();
}

void ToggleSwitchWidget::apply()
{
    const bool configured = flag("checked");
    const bool checked = !isLive() || !m_applied || configured != m_configuredChecked ? configured : m_checked;
    setSwitchStyle(text("switchStyle"));
    if (m_label)
        m_label->setText(text("label"));
    else
        m_checkBox->setText(text("label"));
    m_configuredChecked = configured;
    m_applied = true;
    setChecked(checked);
}

void ToggleSwitchWidget::setChecked(bool checked)
{
    m_checked = checked;
    if (m_switch) {
        const QSignalBlocker blocker(m_switch);
        m_switch->setChecked(checked);
    } else {
        const QSignalBlocker blocker(m_checkBox);
        m_checkBox->setChecked(checked);
    }
}

void ToggleSwitchWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    std::optional<bool> checked;
    if (text("onCondition") == QStringLiteral("等于指定值")) { // i18n: value
        const auto number = bindingFloat(value);
        const auto expected = bindingFloat(text("onValue"));
        if (number && expected) {
            checked = *number == *expected;
        }
    } else {
        checked = bindingBoolean(value);
    }
    if (checked) {
        setChecked(*checked);
    }
}

void ToggleSwitchWidget::checkedEdited(bool checked)
{
    m_checked = checked;
    if (!context())
        return;
    const bool modbus = text("commandMode") == QLatin1String("Modbus");
    const bool s7 = text("commandMode") == QLatin1String("S7");
    const QString commandValue = text(checked ? "onCommand" : "offCommand").trimmed();
    context()->writeModbusSlaves(this, checked ? 1 : 0, modbus || s7 || !commandValue.isEmpty());
    if (s7) {
        const auto command = prop("s7Command").toObject();
        const auto configured = command.value(checked ? QStringLiteral("onValue") : QStringLiteral("offValue"));
        const QString value =
            configured.isUndefined() ? QString::number(checked ? 1 : 0) : configured.toVariant().toString();
        context()->send(CommandEncoder::s7FromProperties(properties(), value));
    } else if (modbus) {
        // Old projects omit mbArea and keep writing a coil. A holding register uses FC06 and 0/1.
        const bool holding = text("mbArea") == ModbusArea::HoldingRegisters;
        QJsonObject props = properties();
        props.insert(QStringLiteral("mbFunc"), holding ? QStringLiteral("06 写单寄存器") // i18n: value
                                                       : QStringLiteral("05 写单线圈")); // i18n: value
        props.insert(QStringLiteral("mbDataType"), QStringLiteral("uint16"));
        const int value = checked ? (holding ? 1 : 0xFF00) : 0;
        context()->send(CommandEncoder::modbusFromProperties(props, QString::number(value)));
    } else {
        if (!commandValue.isEmpty())
            context()->send(CommandEncoder::encode(properties(), commandValue));
    }
    if (flag("notify")) {
        context()->notify(text("notifyTitle"), text("notifyContent"), text("notifyType"), integer("notifyDuration"));
    }
}

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

SliderWidget::SliderWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_slider = new FluentQt::Slider(Qt::Horizontal, this);
    m_minLabel = new FluentQt::CaptionLabel(this);
    m_maxLabel = new FluentQt::CaptionLabel(this);
    m_minLabel->hide();
    m_maxLabel->hide();
    // The labels are in the row from the start, so a hidden one takes no room: with showRange off the
    // slider fills the widget as it did before the labels existed.
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(QMargins());
    row->setSpacing(6);
    row->addWidget(m_minLabel, 0, Qt::AlignVCenter);
    row->addWidget(m_slider, 1);
    row->addWidget(m_maxLabel, 0, Qt::AlignVCenter);
}

// The Fluent slider moves its handle in valueChanged, so the signal stays connected and
// a guard keeps programmatic changes from being sent as commands.
void SliderWidget::apply()
{
    m_quiet = true;
    m_slider->setRange(integer("minimum"), integer("maximum"));
    m_slider->setValue(integer("value"));
    m_quiet = false;
    // Read back from the slider, so the labels show the range it actually has after normalizing.
    m_minLabel->setText(QString::number(m_slider->minimum()));
    m_maxLabel->setText(QString::number(m_slider->maximum()));
    const bool showRange = flag("showRange");
    m_minLabel->setVisible(showRange);
    m_maxLabel->setVisible(showRange);
}

void SliderWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto number = bindingInteger(value)) {
        // Device feedback moves the handle without echoing a command back.
        m_quiet = true;
        m_slider->setValue(*number);
        m_quiet = false;
    }
}

// A drag sends once, when the mouse lets go, instead of a command for every value the handle passes;
// keys and the wheel move in steps and send each one.
// Each of those values also goes into the Modbus slaves' tables, sendOnChange or not.
void SliderWidget::onAttached()
{
    m_sends = flag("sendOnChange") || text("commandMode") == QLatin1String("Modbus") || text("commandMode") == QLatin1String("S7");
    // FluentQt's slider never sets QSlider::isSliderDown(), and a drag that starts on the groove
    // emits no pressed/released signals, so the mouse is watched on the slider and its handle.
    m_slider->installEventFilter(this);
    m_slider->handle()->installEventFilter(this);
    live(connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (!m_quiet && !m_held) {
            sendValue(value);
        }
    }));
}

void SliderWidget::onDetached()
{
    m_slider->removeEventFilter(this);
    m_slider->handle()->removeEventFilter(this);
    m_held = false;
}

bool SliderWidget::eventFilter(QObject *watched, QEvent *event)
{
    const bool left = (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)
                      && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton;
    if (left && event->type() == QEvent::MouseButtonPress) {
        m_held = true;
        m_pressValue = m_slider->value(); // before a press on the groove moves it
    } else if (left && m_held) {
        m_held = false;
        if (m_slider->value() != m_pressValue) {
            sendValue(m_slider->value());
        }
    }
    return HmiWidget::eventFilter(watched, event);
}

void SliderWidget::sendValue(int value)
{
    context()->writeModbusSlaves(this, value, m_sends);
    if (m_sends) {
        context()->send(CommandEncoder::encode(properties(), QString::number(value)));
    }
}

// ---------------------------------------------------------------------------
// SpinBox
// ---------------------------------------------------------------------------

SpinBoxWidget::SpinBoxWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    setNumberMode(false);
}

void SpinBoxWidget::setNumberMode(bool decimal)
{
    if (m_editor && decimal == (m_doubleSpin != nullptr))
        return;
    QAbstractSpinBox *previous = m_editor;
    m_spin = nullptr;
    m_doubleSpin = nullptr;
    if (decimal) {
        m_doubleSpin = new FluentQt::DoubleSpinBox(this);
        connect(m_doubleSpin, &QDoubleSpinBox::valueChanged, this, &SpinBoxWidget::valueEdited);
        m_editor = m_doubleSpin;
    } else {
        m_spin = new FluentQt::SpinBox(this);
        connect(m_spin, &QSpinBox::valueChanged, this, [this](int value) { valueEdited(value); });
        m_editor = m_spin;
    }
    m_editor->setObjectName(QStringLiteral("hmiNumericInput"));
    if (previous) {
        delete layout()->replaceWidget(previous, m_editor);
        delete previous;
    } else {
        static_cast<QHBoxLayout *>(layout())->addWidget(m_editor);
    }
    m_editor->show();
}

void SpinBoxWidget::apply()
{
    const double configured = real("value");
    const double value = !isLive() || !m_applied || configured != m_configuredValue ? configured : m_value;
    setNumberMode(text("numberMode") == QStringLiteral("小数")); // i18n: value
    const QSignalBlocker blocker(m_editor);
    const QString suffix = text("suffix");
    const QString displayedSuffix = suffix.isEmpty() ? QString() : QStringLiteral(" ") + suffix;
    if (m_doubleSpin) {
        m_doubleSpin->setDecimals(std::clamp(integer("decimals"), 0, 6));
        m_doubleSpin->setRange(real("minimum"), real("maximum"));
        const double scale = std::pow(10., m_doubleSpin->decimals());
        m_doubleSpin->setSingleStep(std::max(1., std::round(real("singleStep") * scale)) / scale);
        m_doubleSpin->setSuffix(displayedSuffix);
    } else {
        m_spin->setRange(integer("minimum"), integer("maximum"));
        m_spin->setSingleStep(std::max(1, qRound(real("singleStep"))));
        m_spin->setSuffix(displayedSuffix);
    }
    m_configuredValue = configured;
    m_applied = true;
    setValue(value);
}

void SpinBoxWidget::setValue(double value)
{
    const QSignalBlocker blocker(m_editor);
    if (m_doubleSpin) {
        m_doubleSpin->setValue(value);
        m_value = m_doubleSpin->value();
    } else {
        m_spin->setValue(qRound(value));
        m_value = m_spin->value();
    }
}

void SpinBoxWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (m_doubleSpin) {
        if (const auto number = bindingFloat(value))
            setValue(*number);
    } else {
        if (const auto number = bindingInteger(value))
            setValue(*number);
    }
}

void SpinBoxWidget::valueEdited(double value)
{
    m_value = value;
    if (!context())
        return;
    const QString mode = text("commandMode");
    const bool sendsCommand = flag("sendOnChange") || mode == QLatin1String("Modbus") || mode == QLatin1String("S7");
    context()->writeModbusSlaves(this, value, sendsCommand);
    if (sendsCommand) {
        const QString number =
            m_doubleSpin ? QString::number(value, 'g', QLocale::FloatingPointShortest) : QString::number(int(value));
        context()->send(CommandEncoder::encode(properties(), number));
    }
}

} // namespace fhmi
