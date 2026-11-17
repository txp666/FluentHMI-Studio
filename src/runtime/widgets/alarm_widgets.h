#pragma once
#include "runtime/hmi_widget.h"
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
class QLabel;
class QResizeEvent;
class QTableWidget;
class QTimer;
class QVBoxLayout;
namespace FluentQt {
class ComboBox;
class BodyLabel;
class Dialog;
class FlowLayout;
class PushButton;
class SearchLineEdit;
} // namespace FluentQt
namespace fhmi {
class DateTimeInput;
struct AlarmState;
class AlarmBannerWidget : public HmiWidget
{
  public:
    AlarmBannerWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;

  private:
    void refresh();
    FluentQt::BodyLabel *m_label;
};
class AlarmListWidget : public HmiWidget
{
  public:
    AlarmListWidget(const WidgetMeta &meta, QWidget *parent);
    ~AlarmListWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;
    void resizeEvent(QResizeEvent *event) override;

  private:
    void refresh();
    void exportHistory(bool filtered);
    QJsonArray historyRecords(bool filtered) const;
    QPair<QDateTime, QDateTime> filterTimeBounds() const;
    void updateMode();
    void updateFilters();
    void resetFilters();
    void updateSelection();
    void showFilters(bool visible);
    bool filtersFitInline() const;
    bool matchesFilter(const QString &id, const QString &text, int level, const QDateTime &time,
                       const QDateTime &start, const QDateTime &end, const AlarmState *state,
                       const QString &event = {}) const;
    QVBoxLayout *m_layout;
    FluentQt::FlowLayout *m_bar;
    FluentQt::FlowLayout *m_criteria;
    QPointer<FluentQt::Dialog> m_filterDialog;
    QTableWidget *m_table;
    FluentQt::ComboBox *m_mode;
    FluentQt::PushButton *m_ack;
    QList<QWidget *> m_currentActions;
    FluentQt::PushButton *m_exportFiltered;
    FluentQt::PushButton *m_exportAll;
    FluentQt::PushButton *m_filterButton;
    QWidget *m_filters;
    FluentQt::SearchLineEdit *m_keyword;
    FluentQt::ComboBox *m_level;
    FluentQt::ComboBox *m_state;
    FluentQt::ComboBox *m_event;
    FluentQt::ComboBox *m_timeRange;
    QWidget *m_customTime;
    DateTimeInput *m_from;
    DateTimeInput *m_to;
    FluentQt::BodyLabel *m_timeProblem;
    QTimer *m_filterTimer;
};
} // namespace fhmi
