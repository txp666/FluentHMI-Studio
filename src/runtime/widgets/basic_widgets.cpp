#include "runtime/widgets/basic_widgets.h"

#include "core/frame_templates.h"
#include "core/project_logic.h"
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
#include <QtCore/QScopedValueRollback>
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
    m_caption = text("text");
    periodicSendChanged(isPeriodicSending());
    m_button->setEnabled(flag("enabled"));
}

void PushButtonWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    m_caption = displayString(value);
    periodicSendChanged(isPeriodicSending());
}

void PushButtonWidget::periodicSendChanged(bool running)
{
    m_button->setText(running ? (isPeriodicStarting() ? tr("取消启动") : tr("停止周期发送")) : m_caption);
}

void PushButtonWidget::sendCommand()
{
    if (!context() || stopPeriodicSend()) {
        return;
    }
    const auto encode = [this, props = properties()] {
        return CommandEncoder::buttonFromProperties(props,
            [this](const QString &id) { return context()->inputValue(id); }, nullptr, frameTemplates());
    };
    const auto command = encode();
    const auto notify = [this] {
        if (flag("notify"))
            context()->notify(text("notifyTitle"), text("notifyContent"), text("notifyType"), integer("notifyDuration"));
    };
    if (const int interval = CommandEncoder::periodicSendInterval(properties()); interval > 0) {
        CommandReader readInputs;
        if (CommandEncoder::supportsPeriodicInputs(properties()))
            readInputs = encode;
        requestSend(command.command || !command.error.isEmpty() ? command : CommandResult::failure(tr("请配置发送内容")),
                    notify, {}, interval, std::move(readInputs));
        return;
    }
    if (command.command || !command.error.isEmpty()) {
        if (confirmsSend() || CommandEncoder::usesFrameFields(properties()) || !CommandEncoder::commandInputs(properties()).isEmpty()) {
            requestSend(command, notify);
            return;
        }
        requestSend(command);
    }
    notify();
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
    m_title = new FluentQt::BodyLabel(this);
    m_title->setObjectName(QStringLiteral("hmiInputTitle"));
    m_title->setTextFormat(Qt::PlainText);
    m_edit = new FluentQt::LineEdit(this);
    m_title->setBuddy(m_edit);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(m_title, 0, Qt::AlignVCenter);
    row->addWidget(m_edit, 1);
}

void LineEditWidget::apply()
{
    const QString title = text("title").trimmed();
    m_title->setText(title);
    m_title->setVisible(!title.isEmpty());
    m_edit->setAccessibleName(title);
    m_edit->setPlaceholderText(text("placeholderText"));
    m_edit->setText(text("text"));
    m_edit->setReadOnly(flag("inputOnly") && flag("readOnly"));
}

void LineEditWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    const QSignalBlocker blocker(m_edit);
    m_edit->setText(displayString(value));
}

std::optional<QVariant> LineEditWidget::inputValue() const
{
    const QString parameter = text("parameterName");
    if (context() && !parameter.isEmpty()) {
        const auto shared = context()->inputValue(pointInputId(parameter));
        return shared ? std::optional<QVariant>(shared->value) : std::nullopt;
    }
    return m_edit->text();
}

void LineEditWidget::onAttached()
{
    const QString parameter = text("parameterName");
    if (!parameter.isEmpty()) {
        auto *data = context()->projectData();
        const auto sync = [this, data, parameter] {
            const auto sample = data->sample(parameter);
            const bool valid = sample.quality == DataQuality::Current;
            m_edit->setError(!valid);
            if (valid && (!m_edit->hasFocus() || !m_edit->isModified()))
                m_edit->setText(displayString(sample.value));
        };
        sync();
        live(connect(data, &ProjectData::valuesChanged, this, [parameter, sync](const Message &changes) {
            if (changes.contains(parameter))
                sync();
        }));
        live(connect(m_edit, &QLineEdit::editingFinished, this, [this, data, parameter, sync] {
            if (m_edit->isModified() && data->setParameter(parameter, m_edit->text(), true)) {
                m_edit->setModified(false);
                sync();
            }
        }));
        return;
    }
    live(connect(m_edit, &QLineEdit::returnPressed, this, [this]() {
        if (!widgetSendsCommands(typeName(), properties()))
            return;
        const QString value = m_edit->text().trimmed();
        const QString mode = text("commandMode");
        const auto props = properties();
        if (value.isEmpty() && mode != QLatin1String("Modbus") && mode != QLatin1String("S7")) {
            const auto field = props.value(FrameTemplates::SendValueField).toString();
            const auto settings = FrameTemplates::sendSettings(frameTemplates(), props);
            if (field.isEmpty() || !settings || !FrameTemplates::requiredFields(*settings).contains(field))
                return;
        }
        requestSend(CommandEncoder::encode(props, value, nullptr, frameTemplates()));
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

void ComboBoxWidget::rebuildSelection(const QString &style, const QList<CommandEncoder::SelectionOption> &options)
{
    QWidget *previous = m_selection;
    m_combo = nullptr;
    m_segmented = nullptr;
    m_radioGroup = nullptr;
    m_selectionScroll = nullptr;
    m_selectionStyle = style;
    m_options = options;
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
        for (int i = 0; i < options.size(); ++i)
            m_segmented->addItem(QString::number(i), options.at(i).label);
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
        for (int i = 0; i < options.size(); ++i) {
            auto *radio = new FluentQt::RadioButton(options.at(i).label, content);
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
        for (const auto &option : options)
            m_combo->addItem(option.label);
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
    const auto options = CommandEncoder::selectionOptions(prop("options").toArray());
    QString style = text("selectionStyle");
    if (style != QStringLiteral("分段") && style != QStringLiteral("单选组")) // i18n: value
        style = QStringLiteral("下拉框");                                     // i18n: value
    const int configured = integer("currentIndex");
    const bool optionsChanged = options != m_options;
    const bool reset = !isLive() || !m_applied || configured != m_configuredIndex || optionsChanged;
    int index = reset ? configured : m_currentIndex;
    if (index >= options.size() || (reset && index < 0))
        index = options.isEmpty() ? -1 : 0;
    if (style != m_selectionStyle || optionsChanged)
        rebuildSelection(style, options);
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
                rebuildSelection(m_selectionStyle, m_options);
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
        setCurrentIndex(*index >= 0 && *index < m_options.size() ? *index : -1);
}

void ComboBoxWidget::selectIndex(int index)
{
    if (index < 0) {
        m_currentIndex = -1;
        return;
    }
    if (index >= m_options.size() || index == m_currentIndex)
        return;
    const bool sends = context() && widgetSendsCommands(typeName(), properties());
    if (sends && confirmsSend()) {
        const int previous = m_currentIndex;
        if (m_segmented && previous < 0) {
            // Clearing the selection rebuilds the segmented view after its click handler returns.
            QMetaObject::invokeMethod(this, [this] {
                if (m_currentIndex < 0)
                    setCurrentIndex(-1);
            }, Qt::QueuedConnection);
        } else {
            setCurrentIndex(previous);
        }
        requestSend(CommandEncoder::selectionFromProperties(properties(), m_options.at(index), frameTemplates()), [this, previous, index] {
            if (m_currentIndex == previous)
                setCurrentIndex(index);
        });
        return;
    }
    m_currentIndex = index;
    ensureSelectionVisible();
    if (sends)
        requestSend(CommandEncoder::selectionFromProperties(properties(), m_options.at(index), frameTemplates()));
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
    if (text("onCondition") != QStringLiteral("等于指定值")) { // i18n: value
        if (const auto checked = bindingBoolean(value)) {
            setChecked(*checked);
        }
        return;
    }

    const auto number = bindingFloat(value);
    const auto on = bindingFloat(text("onValue"));
    if (!number || !on) {
        return;
    }
    const bool isOn = *number == *on;
    const QString offText = text("offFeedbackValue").trimmed();
    if (!offText.isEmpty()) {
        const auto off = bindingFloat(offText);
        if (!off || *off == *on || (!isOn && *number != *off)) {
            return;
        }
    }
    setChecked(isOn);
}

void ToggleSwitchWidget::checkedEdited(bool checked)
{
    if (!context()) {
        m_checked = checked;
        return;
    }
    const auto command = CommandEncoder::switchFromProperties(properties(), checked, frameTemplates());
    const bool sends = command.command || !command.error.isEmpty();
    const auto notify = [this] {
        if (flag("notify"))
            context()->notify(text("notifyTitle"), text("notifyContent"), text("notifyType"), integer("notifyDuration"));
    };
    if (sends && confirmsSend()) {
        const bool previous = m_checked;
        setChecked(previous);
        requestSend(command, [this, previous, checked, notify] {
            if (m_checked == previous)
                setChecked(checked);
            notify();
        });
        return;
    }
    m_checked = checked;
    context()->writeModbusSlaves(this, checked ? 1 : 0, sends);
    if (sends)
        requestSend(command);
    notify();
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
    const QScopedValueRollback quiet(m_quiet, true);
    m_slider->setRange(integer("minimum"), integer("maximum"));
    setValue(integer("value"));
    // Read back from the slider, so the labels show the range it actually has after normalizing.
    m_minLabel->setText(QString::number(m_slider->minimum()));
    m_maxLabel->setText(QString::number(m_slider->maximum()));
    const bool showRange = flag("showRange");
    m_minLabel->setVisible(showRange);
    m_maxLabel->setVisible(showRange);
}

void SliderWidget::setValue(int value)
{
    const QScopedValueRollback quiet(m_quiet, true);
    m_slider->setValue(value);
    m_value = m_slider->value();
}

void SliderWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (m_held)
        return;
    if (const auto number = bindingInteger(value))
        setValue(*number);
}

// A drag sends once, when the mouse lets go, instead of a command for every value the handle passes;
// keys and the wheel move in steps and send each one.
// A configured local Modbus binding can write the value when device sending is off.
void SliderWidget::onAttached()
{
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
    const bool sends = widgetSendsCommands(typeName(), properties());
    if (sends && confirmsSend()) {
        const int previous = m_value;
        setValue(previous);
        requestSend(CommandEncoder::encode(properties(), QString::number(value), nullptr, frameTemplates()), [this, previous, value] {
            if (m_value == previous)
                setValue(value);
        });
        return;
    }
    m_value = value;
    context()->writeModbusSlaves(this, value, sends);
    if (sends)
        requestSend(CommandEncoder::encode(properties(), QString::number(value), nullptr, frameTemplates()));
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
    m_editor->setKeyboardTracking(false);
    m_editingText = false;
    auto *line = m_editor->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
    connect(line, &QLineEdit::textEdited, this, [this] { m_editingText = true; });
    connect(m_editor, &QAbstractSpinBox::editingFinished, this, [this] { m_editingText = false; });
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
    if (m_editingText)
        return;
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
    m_editingText = false;
    if (value == m_value)
        return;
    const bool sends = context() && widgetSendsCommands(typeName(), properties());
    const QString number = m_doubleSpin ? QString::number(value, 'g', QLocale::FloatingPointShortest)
                                        : QString::number(int(value));
    if (sends && confirmsSend()) {
        const double previous = m_value;
        setValue(previous);
        requestSend(CommandEncoder::encode(properties(), number, nullptr, frameTemplates()), [this, previous, value] {
            if (m_value == previous)
                setValue(value);
        });
        return;
    }
    m_value = value;
    if (!context())
        return;
    context()->writeModbusSlaves(this, value, sends);
    if (sends)
        requestSend(CommandEncoder::encode(properties(), number, nullptr, frameTemplates()));
}

} // namespace fhmi
