#include "runtime/date_time_input.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/DateTime/CalendarPicker.h>
#include <FluentQtWidgets/DateTime/PickerBase.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CycleListWidget.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QSignalBlocker>
#include <QtWidgets/QHBoxLayout>

namespace fhmi {

DateTimeInput::DateTimeInput(QWidget *parent) : QWidget(parent)
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    row->setSizeConstraint(QLayout::SetMinimumSize);
    m_date = new FluentQt::DateEdit(this);
    m_date->setObjectName(QStringLiteral("dateEdit"));
    m_date->setAccessibleName(tr("日期"));
    m_date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_date->setSymbolVisible(false);
    m_date->setMinimumWidth(m_date->fontMetrics().horizontalAdvance(QStringLiteral("8888-88-88")) + 28);
    m_time = new FluentQt::TimeEdit(this);
    m_time->setObjectName(QStringLiteral("timeEdit"));
    m_time->setAccessibleName(tr("时间"));
    m_time->setDisplayFormat(QStringLiteral("HH:mm:ss"));
    m_time->setSymbolVisible(false);
    m_time->setMinimumWidth(m_time->fontMetrics().horizontalAdvance(QStringLiteral("88:88:88")) + 28);
    for (QDateTimeEdit *edit : {static_cast<QDateTimeEdit *>(m_date), static_cast<QDateTimeEdit *>(m_time)}) {
        edit->setKeyboardTracking(false);
        edit->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    }
    const auto pickerButton = [this](FluentQt::FluentIcon icon, QWidget *edit, const QString &name,
                                     const QString &label) {
        auto *button = new FluentQt::ToolButton(FluentQt::icon(icon), this);
        button->setObjectName(name);
        button->setAccessibleName(label);
        fluentToolTip(button, label);
        keepSquareBeside(button, edit);
        return button;
    };
    auto *calendar = pickerButton(FluentQt::FluentIcon::Calendar, m_date, QStringLiteral("calendarButton"), tr("选择日期"));
    auto *time = pickerButton(FluentQt::FluentIcon::DateTime, m_time, QStringLiteral("timePickerButton"), tr("选择时间"));
    row->addWidget(m_date);
    row->addWidget(calendar);
    row->addSpacing(4);
    row->addWidget(m_time);
    row->addWidget(time);
    setFocusProxy(m_date);
    setDateTime(QDateTime::currentDateTime());
    connect(calendar, &QToolButton::clicked, this, &DateTimeInput::showCalendar);
    connect(time, &QToolButton::clicked, this, &DateTimeInput::showTimePicker);
    connect(m_date, &QDateTimeEdit::dateChanged, this, [this] { emit dateTimeChanged(dateTime()); });
    connect(m_time, &QDateTimeEdit::timeChanged, this, [this] { emit dateTimeChanged(dateTime()); });
}

QDateTime DateTimeInput::dateTime() const { return dateTime(QDateTime::TransitionResolution::PreferBefore); }

QDateTime DateTimeInput::dateTime(QDateTime::TransitionResolution resolution) const
{
    const QDate date = m_date->date();
    const QTime time = m_time->time();
    const QDateTime local(date, time, resolution);
    // A daylight-saving gap must never move the user's entered time to another hour.
    return local.date() == date && local.time() == time ? local : QDateTime();
}

void DateTimeInput::setDateTime(const QDateTime &value)
{
    if (!value.isValid())
        return;
    const QDateTime before = dateTime();
    const QDateTime local = value.toLocalTime();
    const QTime time = local.time();
    const QSignalBlocker dateBlocker(m_date), timeBlocker(m_time);
    m_date->setDate(local.date());
    m_time->setTime(QTime(time.hour(), time.minute(), time.second()));
    if (dateTime() != before)
        emit dateTimeChanged(dateTime());
}

void DateTimeInput::showCalendar()
{
    delete m_calendar;
    m_calendar = new FluentQt::CalendarView(this);
    m_calendar->setObjectName(QStringLiteral("dateCalendar"));
    m_calendar->setWindowTitle(tr("选择日期"));
    m_calendar->setDate(m_date->date());
    connect(m_calendar, &FluentQt::CalendarView::dateChanged, m_date, &QDateTimeEdit::setDate);
    m_calendar->exec(m_date->mapToGlobal(QPoint(0, m_date->height())));
}

void DateTimeInput::showTimePicker()
{
    // Use the same wheel panel as Fluent TimePicker; keep TimeEdit for direct keyboard entry.
    delete m_timePanel;
    m_timePanel = new FluentQt::PickerPanel(this);
    m_timePanel->setObjectName(QStringLiteral("timePickerPanel"));
    m_timePanel->setWindowTitle(tr("选择时间"));
    m_timePanel->setAttribute(Qt::WA_DeleteOnClose);
    m_timePanel->setPanelRole(QStringLiteral("TimePickerPanel"));
    m_timePanel->yesButton()->setAccessibleName(tr("确定"));
    m_timePanel->cancelButton()->setAccessibleName(tr("取消"));
    const QStringList names{tr("时"), tr("分"), tr("秒")};
    for (int column = 0; column < 3; ++column) {
        QStringList values;
        for (int value = 0; value < (column == 0 ? 24 : 60); ++value)
            values.append(QString::number(value).rightJustified(2, QLatin1Char('0')));
        m_timePanel->addColumn(values, 80);
        m_timePanel->column(column)->setAccessibleName(names.at(column));
    }
    m_timePanel->setValue(m_time->time().toString(QStringLiteral("HH:mm:ss")).split(QLatin1Char(':')));
    connect(m_timePanel, &FluentQt::PickerPanel::confirmed, m_time, [this](const QStringList &values) {
        m_time->setTime(QTime(values.at(0).toInt(), values.at(1).toInt(), values.at(2).toInt()));
    });
    m_timePanel->exec(m_time->mapToGlobal(QPoint(0, m_time->height())));
}

} // namespace fhmi
