#include "runtime/widgets/basic_widgets.h"

#include "runtime/runtime_context.h"

#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Slider.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>

#include <QtCore/QSignalBlocker>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QHBoxLayout>

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
    m_button = new FluentQt::PushButton(this);
    fill(this, m_button);
}

void PushButtonWidget::apply()
{
    m_button->setText(text("text"));
    m_button->setEnabled(flag("enabled"));
}

void PushButtonWidget::onAttached()
{
    live(connect(m_button, &QPushButton::clicked, this, [this]() {
        QJsonObject props = properties();
        if (text("commandMode") == QLatin1String("Modbus")) {
            // A button reads by default: its function code falls back to FC03.
            if (!props.contains(QStringLiteral("mbFunc"))) {
                props.insert(QStringLiteral("mbFunc"), QStringLiteral("03 读保持寄存器"));
            }
            context()->send(CommandEncoder::modbusFromProperties(props));
        } else if (const QString command = text("command").trimmed(); !command.isEmpty()) {
            context()->send(CommandEncoder::encode(props, command));
        }
        if (flag("notify")) {
            context()->notify(text("notifyTitle"), text("notifyContent"), text("notifyType"),
                              integer("notifyDuration"));
        }
    }));
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

void LineEditWidget::onAttached()
{
    const bool modbus = text("commandMode") == QLatin1String("Modbus");
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
    m_combo = new FluentQt::ComboBox(this);
    fill(this, m_combo);
}

void ComboBoxWidget::apply()
{
    const QSignalBlocker blocker(m_combo);
    m_combo->clear();
    const QStringList items = nonEmptyLines(text("items"));
    m_combo->addItems(items);
    const int index = integer("currentIndex");
    if (index >= 0 && index < items.size()) {
        m_combo->setCurrentIndex(index);
    }
}

void ComboBoxWidget::onAttached()
{
    if (!flag("sendOnSelect") && text("commandMode") != QLatin1String("Modbus")) {
        return;
    }
    live(connect(m_combo, &FluentQt::ComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        // Each option may have its own command line; without one, the option text is sent.
        QStringList commands = text("commands").split(QLatin1Char('\n'));
        for (QString &command : commands) {
            command = command.trimmed();
        }
        const QString raw = index < commands.size() && !commands.at(index).isEmpty() ? commands.at(index)
                                                                                     : m_combo->currentText();
        if (!raw.isEmpty()) {
            context()->send(CommandEncoder::encode(properties(), raw));
        }
    }));
}

// ---------------------------------------------------------------------------
// ToggleSwitch
// ---------------------------------------------------------------------------

ToggleSwitchWidget::ToggleSwitchWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    m_label = new FluentQt::BodyLabel(this);
    m_switch = new FluentQt::SwitchButton(this);
    layout->addWidget(m_label);
    layout->addStretch();
    layout->addWidget(m_switch);
}

void ToggleSwitchWidget::apply()
{
    m_label->setText(text("label"));
    const QSignalBlocker blocker(m_switch);
    m_switch->setChecked(flag("checked"));
}

void ToggleSwitchWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto checked = bindingBoolean(value)) {
        const QSignalBlocker blocker(m_switch);
        m_switch->setChecked(*checked);
    }
}

void ToggleSwitchWidget::onAttached()
{
    const bool modbus = text("commandMode") == QLatin1String("Modbus");
    const bool hasCommands = !text("onCommand").trimmed().isEmpty() || !text("offCommand").trimmed().isEmpty();
    if (modbus || hasCommands) {
        live(connect(m_switch, &FluentQt::SwitchButton::checkedChanged, this, [this, modbus](bool checked) {
            if (modbus) {
                // Modbus switches always write a single coil (FC05).
                QJsonObject props = properties();
                props.insert(QStringLiteral("mbFunc"), QStringLiteral("05 写单线圈"));
                context()->send(CommandEncoder::modbusFromProperties(props, QString::number(checked ? 0xFF00 : 0)));
                return;
            }
            const QString command = text(checked ? "onCommand" : "offCommand").trimmed();
            if (!command.isEmpty()) {
                context()->send(CommandEncoder::encode(properties(), command));
            }
        }));
    }
    if (flag("notify")) {
        live(connect(m_switch, &FluentQt::SwitchButton::checkedChanged, this, [this]() {
            context()->notify(text("notifyTitle"), text("notifyContent"), text("notifyType"),
                              integer("notifyDuration"));
        }));
    }
}

// ---------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------

SliderWidget::SliderWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_slider = new FluentQt::Slider(Qt::Horizontal, this);
    fill(this, m_slider);
}

// The Fluent slider moves its handle in valueChanged, so the signal stays connected and
// a guard keeps programmatic changes from being sent as commands.
void SliderWidget::apply()
{
    m_quiet = true;
    m_slider->setRange(integer("minimum"), integer("maximum"));
    m_slider->setValue(integer("value"));
    m_quiet = false;
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
void SliderWidget::onAttached()
{
    if (!flag("sendOnChange") && text("commandMode") != QLatin1String("Modbus")) {
        return;
    }
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
    context()->send(CommandEncoder::encode(properties(), QString::number(value)));
}

// ---------------------------------------------------------------------------
// SpinBox
// ---------------------------------------------------------------------------

SpinBoxWidget::SpinBoxWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_spin = new FluentQt::SpinBox(this);
    fill(this, m_spin);
}

void SpinBoxWidget::apply()
{
    const QSignalBlocker blocker(m_spin);
    m_spin->setRange(integer("minimum"), integer("maximum"));
    m_spin->setValue(integer("value"));
    const QString suffix = text("suffix");
    m_spin->setSuffix(suffix.isEmpty() ? QString() : QStringLiteral(" ") + suffix);
}

void SpinBoxWidget::applyBoundValue(const QVariant &value, const BindingEntry &)
{
    if (const auto number = bindingInteger(value)) {
        const QSignalBlocker blocker(m_spin);
        m_spin->setValue(*number);
    }
}

void SpinBoxWidget::onAttached()
{
    if (!flag("sendOnChange") && text("commandMode") != QLatin1String("Modbus")) {
        return;
    }
    live(connect(m_spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        context()->send(CommandEncoder::encode(properties(), QString::number(value)));
    }));
}

} // namespace fhmi
