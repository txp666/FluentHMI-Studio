#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

namespace FluentQt {
class CalendarView;
class DateEdit;
class PickerPanel;
class TimeEdit;
} // namespace FluentQt

namespace fhmi {

// Local date/time entry shared by range endpoints: editable fields and Fluent popups
// write the same value, including seconds.
class DateTimeInput : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QDateTime dateTime READ dateTime WRITE setDateTime NOTIFY dateTimeChanged)

  public:
    explicit DateTimeInput(QWidget *parent = nullptr);
    QDateTime dateTime() const;
    QDateTime dateTime(QDateTime::TransitionResolution resolution) const;
    void setDateTime(const QDateTime &value);

  signals:
    void dateTimeChanged(const QDateTime &value);

  private:
    void showCalendar();
    void showTimePicker();
    FluentQt::DateEdit *m_date;
    FluentQt::TimeEdit *m_time;
    QPointer<FluentQt::CalendarView> m_calendar;
    QPointer<FluentQt::PickerPanel> m_timePanel;
};

} // namespace fhmi
