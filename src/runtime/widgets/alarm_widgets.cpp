#include "runtime/widgets/alarm_widgets.h"
#include "runtime/date_time_input.h"
#include "runtime/field_help.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/display_widgets.h"
#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/Layout/FlowLayout.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QScreen>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGraphicsProxyWidget>
#include <QtWidgets/QGraphicsScene>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {
namespace {
QColor severityColor(int level) { return QColor(level >= 3 ? "#d13438" : level == 2 ? "#b96a00" : "#0078d4"); }
QString eventText(const QString &event)
{
    if (event == QLatin1String("raised"))
        return QCoreApplication::translate("fhmi", "触发");
    if (event == QLatin1String("cleared"))
        return QCoreApplication::translate("fhmi", "恢复");
    if (event == QLatin1String("acknowledged"))
        return QCoreApplication::translate("fhmi", "确认");
    if (event == QLatin1String("silenced"))
        return QCoreApplication::translate("fhmi", "消音");
    return event;
}
QList<QWidget *> controls(HmiWidget *host, QLayout *bar)
{
    auto *ack = new FluentQt::PushButton(QCoreApplication::translate("fhmi", "确认全部"), host);
    auto *mute = new FluentQt::PushButton(QCoreApplication::translate("fhmi", "消音"), host);
    ack->setObjectName(QStringLiteral("alarmAcknowledgeAll"));
    mute->setObjectName(QStringLiteral("alarmSilence"));
    bar->addWidget(ack);
    bar->addWidget(mute);
    QObject::connect(ack, &QPushButton::clicked, host, [host] {
        if (host->context())
            host->context()->projectData()->acknowledge();
    });
    QObject::connect(mute, &QPushButton::clicked, host, [host] {
        if (host->context())
            host->context()->projectData()->silence();
    });
    return {ack, mute};
}

QWidget *visibleWindow(QWidget *widget)
{
    QWidget *window = widget->window();
    // The exported canvas is scaled within a graphics view. Give the native dialog
    // its real window so it uses screen coordinates and its pickers behave normally.
    if (auto *proxy = window->graphicsProxyWidget(); proxy && proxy->scene()) {
        const auto views = proxy->scene()->views();
        if (!views.isEmpty())
            window = views.first()->window();
    }
    return window;
}
} // namespace
AlarmBannerWidget::AlarmBannerWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *box = new QHBoxLayout(this);
    box->setContentsMargins(10, 6, 10, 6);
    m_label = new FluentQt::BodyLabel(this);
    m_label->setWordWrap(true);
    box->addWidget(m_label, 1);
    controls(this, box);
}
void AlarmBannerWidget::apply() { refresh(); }
void AlarmBannerWidget::onAttached()
{
    live(connect(context()->projectData(), &ProjectData::alarmsChanged, this, [this] { refresh(); }));
    refresh();
}
void AlarmBannerWidget::refresh()
{
    if (!context()) {
        m_label->setText(tr("报警栏 · 运行后显示工程报警"));
        return;
    }
    int active = 0, pending = 0, unknown = 0, level = 0;
    QString latest;
    QDateTime when;
    for (const auto &s : context()->projectData()->alarms()) {
        if (!s.known)
            ++unknown;
        if (s.active)
            ++active;
        if (!s.acknowledged)
            ++pending;
        if (s.outstanding() && (s.level > level || (s.level == level && s.raisedAt >= when))) {
            level = s.level;
            when = s.raisedAt;
            latest = s.text;
        }
    }
    QString label = tr("%1 条活动 · %2 条未确认").arg(active).arg(pending);
    if (level)
        label += tr(" · 最高 %1 级：%2").arg(level).arg(latest);
    if (unknown)
        label += tr(" · %1 条数据未知").arg(unknown);
    m_label->setText(label);
    m_label->setTextColor(level ? severityColor(level) : QColor("#273549"),
                          level ? severityColor(level).lighter(150) : QColor("#e8edf4"));
}
AlarmListWidget::AlarmListWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(6, 6, 6, 6);
    m_layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_bar = new FluentQt::FlowLayout(0, 6, 4);
    m_layout->addLayout(m_bar);
    m_mode = new FluentQt::ComboBox(this);
    m_mode->setObjectName(QStringLiteral("alarmMode"));
    m_mode->addItems({tr("当前报警"), tr("历史记录")});
    m_bar->addWidget(m_mode);
    m_ack = new FluentQt::PushButton(tr("确认所选"), this);
    m_ack->setObjectName(QStringLiteral("alarmAcknowledgeSelected"));
    m_bar->addWidget(m_ack);
    m_currentActions = controls(this, m_bar);
    m_currentActions.prepend(m_ack);
    m_filterButton = new FluentQt::PushButton(tr("筛选"), this);
    m_filterButton->setObjectName(QStringLiteral("alarmFilterToggle"));
    m_filterButton->setCheckable(true);
    m_bar->addWidget(m_filterButton);
    m_exportFiltered = new FluentQt::PushButton(tr("导出筛选结果"), this);
    m_exportFiltered->setObjectName(QStringLiteral("alarmExportFiltered"));
    m_bar->addWidget(m_exportFiltered);
    m_exportAll = new FluentQt::PushButton(tr("导出全部历史"), this);
    m_exportAll->setObjectName(QStringLiteral("alarmExportAllHistory"));
    fluentToolTip(m_exportAll, tr("忽略当前筛选，导出全部历史记录。"));
    m_bar->addWidget(m_exportAll);

    m_filters = new QWidget(this);
    m_filters->setObjectName(QStringLiteral("alarmFilterPanel"));
    auto *filterLayout = new QVBoxLayout(m_filters);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setSpacing(4);
    m_criteria = new FluentQt::FlowLayout(0, 6, 4);
    filterLayout->addLayout(m_criteria);
    m_keyword = new FluentQt::SearchLineEdit(m_filters);
    m_keyword->setObjectName(QStringLiteral("alarmFilterKeyword"));
    m_keyword->setPlaceholderText(tr("名称或报警文字"));
    m_keyword->setFixedWidth(190);
    m_criteria->addWidget(m_keyword);
    m_level = new FluentQt::ComboBox(m_filters);
    m_level->setObjectName(QStringLiteral("alarmFilterLevel"));
    m_level->addItems({tr("全部级别"), tr("1 级"), tr("2 级"), tr("3 级")});
    m_criteria->addWidget(m_level);
    m_state = new FluentQt::ComboBox(m_filters);
    m_state->setObjectName(QStringLiteral("alarmFilterState"));
    m_state->addItems(
        {tr("全部状态"), tr("活动"), tr("未确认"), tr("已恢复"), tr("数据未知"), tr("已消音"), tr("已确认")});
    m_criteria->addWidget(m_state);
    m_event = new FluentQt::ComboBox(m_filters);
    m_event->setObjectName(QStringLiteral("alarmFilterEvent"));
    m_event->addItem(tr("全部事件"), QIcon(), QString());
    for (const QString &event : {QStringLiteral("raised"), QStringLiteral("cleared"), QStringLiteral("acknowledged"),
                                 QStringLiteral("silenced")})
        m_event->addItem(eventText(event), QIcon(), event);
    m_criteria->addWidget(m_event);
    m_timeRange = new FluentQt::ComboBox(m_filters);
    m_timeRange->setObjectName(QStringLiteral("alarmFilterTimeRange"));
    m_timeRange->addItems({tr("不限时间"), tr("最近 1 小时"), tr("最近 24 小时"), tr("最近 7 天"), tr("自定义时间")});
    m_criteria->addWidget(m_timeRange);
    auto *reset = new FluentQt::PushButton(tr("重置筛选"), m_filters);
    reset->setObjectName(QStringLiteral("alarmFilterReset"));
    m_criteria->addWidget(reset);
    m_criteria->addWidget(createFieldHelpButton(FieldHelpTopic::AlarmFilters, m_filters));
    m_customTime = new QWidget(m_filters);
    auto *dates = new FluentQt::FlowLayout(0, 6, 4);
    m_customTime->setLayout(dates);
    const QDateTime now = QDateTime::currentDateTime();
    const auto dateEditor = [this, dates](const QString &objectName, const QString &label, const QDateTime &value) {
        auto *field = new QWidget(m_customTime);
        auto *row = new QHBoxLayout(field);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        row->addWidget(new FluentQt::BodyLabel(label, field));
        auto *edit = new DateTimeInput(field);
        edit->setObjectName(objectName);
        edit->setAccessibleName(label);
        edit->setDateTime(value);
        row->addWidget(edit);
        dates->addWidget(field);
        return edit;
    };
    m_from = dateEditor(QStringLiteral("alarmFilterFrom"), tr("起始"), now.addDays(-1));
    m_to = dateEditor(QStringLiteral("alarmFilterTo"), tr("结束"), now);
    filterLayout->addWidget(m_customTime);
    m_timeProblem = new FluentQt::BodyLabel(tr("起始时间不能晚于结束时间"), m_filters);
    m_timeProblem->setObjectName(QStringLiteral("alarmFilterTimeProblem"));
    m_timeProblem->setTextColor(QColor("#d13438"), QColor("#ff99a4"));
    m_timeProblem->setWordWrap(true);
    filterLayout->addWidget(m_timeProblem);
    m_layout->addWidget(m_filters);
    m_filters->hide();

    m_table = new FluentQt::TableWidget(this);
    m_table->setObjectName(QStringLiteral("alarmTable"));
    m_table->setColumnCount(5);
    m_layout->addWidget(m_table, 1);
    m_table->setHorizontalHeaderLabels({tr("时间"), tr("级别"), tr("名称"), tr("状态"), tr("报警文字")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(36);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_filterTimer = new QTimer(this);
    m_filterTimer->setInterval(60000);
    connect(m_filterTimer, &QTimer::timeout, this, [this] { refresh(); });
    connect(m_filterButton, &QPushButton::toggled, this, &AlarmListWidget::showFilters);
    const auto changed = [this] {
        updateFilters();
        refresh();
    };
    connect(m_keyword, &QLineEdit::textChanged, this, changed);
    for (FluentQt::ComboBox *combo : {m_level, m_state, m_event, m_timeRange})
        connect(combo, &FluentQt::ComboBox::currentIndexChanged, this, changed);
    connect(m_from, &DateTimeInput::dateTimeChanged, this, changed);
    connect(m_to, &DateTimeInput::dateTimeChanged, this, changed);
    connect(reset, &QPushButton::clicked, this, [this] { resetFilters(); });
    connect(m_mode, &FluentQt::ComboBox::currentIndexChanged, this, [this] {
        m_table->setCurrentCell(-1, -1);
        m_table->clearSelection();
        updateMode();
        updateFilters();
        refresh();
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] { updateSelection(); });
    connect(m_ack, &QPushButton::clicked, this, [this] {
        QTableWidgetItem *item = m_table->item(m_table->currentRow(), 2);
        if (context() && m_mode->currentIndex() == 0 && item && item->isSelected())
            context()->projectData()->acknowledge(item->data(Qt::UserRole).toString());
    });
    connect(m_exportFiltered, &QPushButton::clicked, this, [this] { exportHistory(true); });
    connect(m_exportAll, &QPushButton::clicked, this, [this] { exportHistory(false); });
    updateMode();
    updateFilters();
    updateSelection();
}
AlarmListWidget::~AlarmListWidget() { delete m_filterDialog; }

void AlarmListWidget::apply() { refresh(); }
void AlarmListWidget::onAttached()
{
    live(connect(context()->projectData(), &ProjectData::alarmsChanged, this, [this] { refresh(); }));
    live(connect(context()->projectData(), &ProjectData::historyChanged, this, [this] {
        if (m_mode->currentIndex() == 1)
            refresh();
    }));
    updateFilters();
    refresh();
}
void AlarmListWidget::onDetached()
{
    m_filterTimer->stop();
    m_filterButton->setChecked(false);
}

void AlarmListWidget::resizeEvent(QResizeEvent *event)
{
    HmiWidget::resizeEvent(event);
    if (m_filterButton->isChecked() && !m_filterDialog && !filtersFitInline())
        m_filterButton->setChecked(false);
}

bool AlarmListWidget::filtersFitInline() const
{
    const auto margins = m_layout->contentsMargins();
    const int width = this->width() - margins.left() - margins.right();
    const int spacing = m_filters->layout()->spacing();
    const int filterHeight = m_criteria->heightForWidth(width) +
                             m_customTime->layout()->totalHeightForWidth(width) +
                             m_timeProblem->sizeHint().height() + 2 * spacing;
    // Reserve a full visible result row even with the custom time editors displayed.
    const int tableHeight = m_table->horizontalHeader()->sizeHint().height() +
                            m_table->verticalHeader()->defaultSectionSize() + 2 * m_table->frameWidth();
    return height() >= m_bar->heightForWidth(width) + filterHeight + tableHeight +
                           2 * m_layout->spacing() + margins.top() + margins.bottom();
}

void AlarmListWidget::showFilters(bool visible)
{
    if (!visible) {
        if (m_filterDialog)
            m_filterDialog->reject();
        m_filters->hide();
        return;
    }
    if (filtersFitInline()) {
        m_filters->show();
        return;
    }

    auto *host = visibleWindow(this);
    auto *dialog = new FluentQt::Dialog(tr("报警筛选"), {}, host);
    dialog->setObjectName(QStringLiteral("alarmFilterDialog"));
    dialog->hideCancelButton();
    dialog->contentLabel()->hide();
    dialog->yesButton()->setText(tr("关闭"));
    dialog->setWindowModality(Qt::WindowModal);
    auto *scroll = new FluentQt::ScrollArea(dialog);
    scroll->enableTransparentBackground();
    scroll->setWidgetResizable(true);
    const QRect available = host->screen()->availableGeometry();
    scroll->setMinimumSize(QSize(520, 190).boundedTo(available.size() - QSize(108, 185)));
    scroll->setWidget(m_filters);
    m_filters->show();
    dialog->viewLayout()->addWidget(scroll, 1);
    connect(dialog, &QDialog::finished, this, [this, scroll, dialog] {
        scroll->takeWidget();
        m_layout->insertWidget(1, m_filters);
        m_filters->hide();
        const QSignalBlocker blocker(m_filterButton);
        m_filterButton->setChecked(false);
        m_filterDialog = nullptr;
        dialog->deleteLater();
    });
    m_filterDialog = dialog;
    dialog->show();
    dialog->move(std::clamp(dialog->x(), available.left(), std::max(available.left(), available.right() - dialog->width() + 1)),
                 std::clamp(dialog->y(), available.top(), std::max(available.top(), available.bottom() - dialog->height() + 1)));
}

void AlarmListWidget::updateSelection()
{
    QTableWidgetItem *item = m_table->item(m_table->currentRow(), 2);
    m_ack->setEnabled(context() && m_mode->currentIndex() == 0 && item && item->isSelected());
}
void AlarmListWidget::updateMode()
{
    const bool history = m_mode->currentIndex() == 1;
    for (QWidget *action : m_currentActions) {
        action->setVisible(!history);
        action->setEnabled(!history);
    }
    m_exportFiltered->setVisible(history);
    m_exportAll->setVisible(history);
    m_table->horizontalHeaderItem(3)->setText(history ? tr("事件") : tr("状态"));
}
void AlarmListWidget::updateFilters()
{
    const bool history = m_mode->currentIndex() == 1;
    m_state->setVisible(!history);
    m_event->setVisible(history);
    const bool custom = m_timeRange->currentIndex() == 4;
    m_customTime->setVisible(custom);
    const QDateTime from = custom ? m_from->dateTime() : QDateTime();
    const QDateTime to = custom ? m_to->dateTime() : QDateTime();
    const bool invalidLocalTime = custom && (!from.isValid() || !to.isValid());
    m_timeProblem->setText(invalidLocalTime ? tr("此本地时间不存在，请调整起止时间") : tr("起始时间不能晚于结束时间"));
    m_timeProblem->setVisible(custom && (invalidLocalTime || from > to));
    const int active = int(!m_keyword->text().trimmed().isEmpty()) + int(m_level->currentIndex() != 0) +
                       int((history ? m_event : m_state)->currentIndex() != 0) + int(m_timeRange->currentIndex() != 0);
    m_filterButton->setText(active ? tr("筛选 · %1 项").arg(active) : tr("筛选"));
    if (context() && m_timeRange->currentIndex() > 0 && m_timeRange->currentIndex() < 4)
        m_filterTimer->start();
    else
        m_filterTimer->stop();
}
void AlarmListWidget::resetFilters()
{
    const QSignalBlocker keyword(m_keyword), level(m_level), state(m_state), event(m_event), time(m_timeRange);
    m_keyword->clear();
    for (FluentQt::ComboBox *combo : {m_level, m_state, m_event, m_timeRange})
        combo->setCurrentIndex(0);
    updateFilters();
    refresh();
}
bool AlarmListWidget::matchesFilter(const QString &id, const QString &text, int level, const QDateTime &time,
                                    const QDateTime &start, const QDateTime &end, const AlarmState *state,
                                    const QString &event) const
{
    const QString keyword = m_keyword->text().trimmed();
    if (!keyword.isEmpty() && !id.contains(keyword, Qt::CaseInsensitive) &&
        !text.contains(keyword, Qt::CaseInsensitive))
        return false;
    if (m_level->currentIndex() != 0 && level != m_level->currentIndex())
        return false;
    if (state) {
        switch (m_state->currentIndex()) {
        case 1:
            if (!state->known || !state->active)
                return false;
            break;
        case 2:
            if (state->acknowledged)
                return false;
            break;
        case 3:
            if (!state->known || state->active)
                return false;
            break;
        case 4:
            if (state->known)
                return false;
            break;
        case 5:
            if (!state->silenced)
                return false;
            break;
        case 6:
            if (!state->outstanding() || !state->acknowledged)
                return false;
            break;
        }
    } else if (m_event->currentIndex() != 0 && m_event->currentData().toString() != event) {
        return false;
    }
    return m_timeRange->currentIndex() == 0 ||
           (time.isValid() && start.isValid() && end.isValid() && time >= start && time < end);
}

QPair<QDateTime, QDateTime> AlarmListWidget::filterTimeBounds() const
{
    const int range = m_timeRange->currentIndex();
    if (range == 4) {
        const QDateTime start = m_from->dateTime();
        const QDateTime last = m_to->dateTime();
        // Include both occurrences of a repeated local time and the entire displayed end second.
        if (start.isValid() && last.isValid() && start <= last)
            return {start, m_to->dateTime(QDateTime::TransitionResolution::PreferAfter).addSecs(1)};
    } else if (range > 0) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const qint64 seconds = range == 1 ? 3600 : range == 2 ? 86400 : 7 * 86400;
        return {now.addSecs(-seconds), now.addMSecs(1)};
    }
    return {};
}

QJsonArray AlarmListWidget::historyRecords(bool filtered) const
{
    const auto records = context()->projectData()->history();
    if (!filtered)
        return records;
    const auto [start, end] = filterTimeBounds();
    QJsonArray matches;
    for (const auto &record : records) {
        const auto h = record.toObject();
        const auto time = QDateTime::fromString(h.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs);
        if (matchesFilter(h.value(QStringLiteral("id")).toString(), h.value(QStringLiteral("text")).toString(),
                          h.value(QStringLiteral("level")).toInt(), time, start, end, nullptr,
                          h.value(QStringLiteral("event")).toString()))
            matches.append(record);
    }
    return matches;
}

void AlarmListWidget::refresh()
{
    QTableWidgetItem *selection = m_table->item(m_table->currentRow(), 2);
    const QString selected =
        selection && selection->isSelected() ? selection->data(Qt::UserRole + 1).toString() : QString();
    const QSignalBlocker tableSignals(m_table);
    m_table->setRowCount(0);
    int selectedRow = -1;
    if (context()) {
        const auto add = [&](const QDateTime &time, int level, const QString &id, const QString &state,
                             const QString &text, const QString &key) {
            const int row = m_table->rowCount();
            m_table->insertRow(row);
            const QStringList values{time.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                     tr("%1 级").arg(level), id, state, text};
            for (int i = 0; i < values.size(); ++i)
                m_table->setItem(row, i, new QTableWidgetItem(values[i]));
            m_table->item(row, 2)->setData(Qt::UserRole, id);
            m_table->item(row, 2)->setData(Qt::UserRole + 1, key);
            const QColor color = severityColor(level);
            m_table->item(row, 1)->setForeground(isDarkTheme() ? color.lighter(150) : color);
            if (key == selected)
                selectedRow = row;
        };
        if (m_mode->currentIndex() == 0) {
            const auto [start, end] = filterTimeBounds();
            for (const auto &s : context()->projectData()->alarms()) {
                if ((!s.outstanding() && s.known) || !matchesFilter(s.id, s.text, s.level, s.raisedAt, start, end, &s))
                    continue;
                QString state = !s.known ? tr("数据未知") : s.active ? tr("活动") : tr("已恢复");
                if (s.outstanding())
                    state += s.acknowledged ? tr(" · 已确认") : tr(" · 未确认");
                if (s.silenced)
                    state += tr(" · 已消音");
                add(s.raisedAt, s.level, s.id, state, s.text, QStringLiteral("current:") + s.id);
            }
        } else {
            const auto records = historyRecords(true);
            for (auto it = records.constEnd(); it != records.constBegin();) {
                const auto h = (*--it).toObject();
                const QDateTime time =
                    QDateTime::fromString(h.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs);
                const int level = h.value(QStringLiteral("level")).toInt();
                const QString id = h.value(QStringLiteral("id")).toString();
                const QString text = h.value(QStringLiteral("text")).toString();
                const QString event = h.value(QStringLiteral("event")).toString();
                const QString entryId = h.value(QStringLiteral("entryId")).toString();
                add(time, level, id, eventText(event), text,
                    entryId.isEmpty() ? QString::fromUtf8(QJsonDocument(h).toJson(QJsonDocument::Compact)) : entryId);
            }
        }
    }
    m_table->clearSelection();
    m_table->setCurrentCell(-1, -1);
    if (selectedRow >= 0) {
        m_table->setCurrentCell(selectedRow, 2);
        m_table->selectRow(selectedRow);
    }
    updateSelection();
}
void AlarmListWidget::exportHistory(bool filtered)
{
    if (!context())
        return;
    const auto records = historyRecords(filtered);
    const QString path = QFileDialog::getSaveFileName(visibleWindow(this), tr("导出报警历史"), QStringLiteral("alarms.csv"),
                                                      QStringLiteral("CSV (*.csv)"));
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    QByteArray bytes("\xEF\xBB\xBFtime,level,id,event,text\r\n");
    for (const auto &record : records) {
        const auto h = record.toObject();
        QStringList row;
        for (const char *key : {"time", "level", "id", "event", "text"}) {
            QString value = h.value(QLatin1String(key)).toVariant().toString();
            if (QLatin1String(key) == QLatin1String("time")) {
                const QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
                if (time.isValid())
                    value = time.toUTC().toString(Qt::ISODateWithMs);
            }
            value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            row.append(QLatin1Char('"') + value + QLatin1Char('"'));
        }
        bytes += row.join(QLatin1Char(',')).toUtf8() + "\r\n";
    }
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        if (context())
            context()->notify(tr("导出报警历史"), file.errorString(), NotifyType::Error);
    }
}
} // namespace fhmi
