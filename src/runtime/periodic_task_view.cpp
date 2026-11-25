#include "runtime/periodic_task_view.h"

#include "runtime/app_dialog.h"
#include "runtime/periodic_sender.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {
namespace {

QString tr(const char *text) { return QCoreApplication::translate("fhmi", text); }

void openPeriodicTasks(PeriodicSender *sender, QWidget *window)
{
    if (!sender) {
        return;
    }
    if (window) {
        if (auto *existing = window->findChild<AppDialog *>(QStringLiteral("periodicTasksDialog"),
                                                            Qt::FindDirectChildrenOnly)) {
            if (existing->isVisible()) {
                existing->raise();
                existing->activateWindow();
                return;
            }
        }
    }
    auto *dialog = new AppDialog(tr("周期任务"), window);
    dialog->setObjectName(QStringLiteral("periodicTasksDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPreferredSize(QSize(840, 455));
    dialog->hideCancelButton();
    dialog->yesButton()->setText(tr("关闭"));
    QObject::connect(sender, &QObject::destroyed, dialog, &QDialog::reject);

    auto *table = new FluentQt::TableWidget(dialog);
    table->setObjectName(QStringLiteral("periodicTasksTable"));
    enableHorizontalWheelScroll(table);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels(
        {tr("任务"), tr("发送目标"), tr("间隔(ms)"), tr("状态"), tr("已提交"), tr("跳过")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(42);
    auto *header = table->horizontalHeader();
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setColumnWidth(2, 86);
    table->setColumnWidth(3, 108);
    table->setColumnWidth(4, 78);
    table->setColumnWidth(5, 68);
    dialog->viewLayout()->addWidget(table, 1);

    auto *empty = new FluentQt::BodyLabel(tr("当前没有周期任务"), dialog);
    dialog->viewLayout()->addWidget(empty);
    auto *contentLabel = new FluentQt::CaptionLabel(tr("最近组帧内容"), dialog);
    dialog->viewLayout()->addWidget(contentLabel);
    auto *content = new FluentQt::PlainTextEdit(dialog);
    content->setReadOnly(true);
    content->setAccessibleName(contentLabel->text());
    content->setFixedHeight(72);
    dialog->viewLayout()->addWidget(content);
    auto *inputError = new FluentQt::CaptionLabel(dialog);
    inputError->setWordWrap(true);
    inputError->setTextFormat(Qt::PlainText);
    inputError->setTextColor(QColor(0xC42B1C), QColor(0xFF99A4));
    dialog->viewLayout()->addWidget(inputError);

    auto *actions = new QHBoxLayout;
    auto *stop = new FluentQt::PushButton(tr("停止所选"), dialog);
    stop->setObjectName(QStringLiteral("stopPeriodicTask"));
    auto *stopAll = new FluentQt::PushButton(tr("停止全部"), dialog);
    stopAll->setObjectName(QStringLiteral("stopAllPeriodicTasks"));
    actions->addWidget(stop);
    actions->addWidget(stopAll);
    actions->addStretch();
    dialog->viewLayout()->addLayout(actions);

    const auto selected = [table]() -> QTableWidgetItem * {
        const auto rows = table->selectionModel()->selectedRows(0);
        return rows.isEmpty() ? nullptr : table->item(rows.first().row(), 0);
    };
    const auto selection = [selected, contentLabel, content, inputError, stop] {
        const auto *item = selected();
        const QString text = item ? item->data(Qt::UserRole + 1).toString() : QString();
        if (text != content->toPlainText())
            content->setPlainText(text);
        contentLabel->setVisible(item && !text.isEmpty());
        content->setVisible(item && !text.isEmpty());
        const QString error = item ? item->data(Qt::UserRole + 2).toString() : QString();
        inputError->setText(error);
        inputError->setVisible(!error.isEmpty());
        stop->setEnabled(item != nullptr);
    };
    const auto refresh = [sender, table, empty, stopAll, selected, selection] {
        const auto token = selected() ? selected()->data(Qt::UserRole).toULongLong() : 0;
        const auto tasks = sender->tasks();
        const QSignalBlocker blocked(table);
        table->setRowCount(int(tasks.size()));
        int selectedRow = -1;
        for (int row = 0; row < tasks.size(); ++row) {
            const auto &task = tasks.at(row);
            QString state = task.paused ? tr("等待接管") : task.submitted ? tr("运行中") : tr("等待首帧");
            if (!task.inputError.isEmpty())
                state = tr("等待有效输入");
            const QStringList cells{task.title, task.targets.simplified(), QString::number(task.intervalMs),
                                    state, QString::number(task.submitted), QString::number(task.skipped)};
            for (int column = 0; column < table->columnCount(); ++column) {
                auto *cell = table->item(row, column);
                if (!cell) {
                    cell = new QTableWidgetItem;
                    table->setItem(row, column, cell);
                }
                if (cell->text() != cells.at(column))
                    cell->setText(cells.at(column));
            }
            auto *item = table->item(row, 0);
            item->setData(Qt::UserRole, QVariant::fromValue(task.token));
            item->setData(Qt::UserRole + 1, task.content);
            item->setData(Qt::UserRole + 2, task.inputError);
            item->setToolTip(task.id);
            table->item(row, 1)->setToolTip(task.targets);
            if (task.token == token)
                selectedRow = row;
        }
        if (selectedRow < 0)
            table->selectionModel()->clear();
        else if (table->currentRow() != selectedRow || !selected())
            table->selectRow(selectedRow);
        empty->setVisible(tasks.isEmpty());
        stopAll->setEnabled(!tasks.isEmpty());
        selection();
    };
    QObject::connect(table, &QTableWidget::itemSelectionChanged, dialog, selection);
    QObject::connect(stop, &QPushButton::clicked, dialog, [sender, table, selected] {
        if (const auto *item = selected()) {
            const auto token = item->data(Qt::UserRole).toULongLong();
            table->selectionModel()->clear();
            sender->stopTask(token);
        }
    });
    QObject::connect(stopAll, &QPushButton::clicked, sender, &PeriodicSender::stopAll);
    QObject::connect(sender, &PeriodicSender::changed, dialog, refresh);
    auto *updates = new QTimer(dialog);
    QObject::connect(updates, &QTimer::timeout, dialog, refresh);
    updates->start(250);
    refresh();
    if (table->rowCount() > 0)
        table->selectRow(0);
    dialog->open();
}

} // namespace

QPushButton *createPeriodicTasksButton(PeriodicSender *sender, QWidget *parent)
{
    auto *button = new FluentQt::PushButton(parent);
    button->setObjectName(QStringLiteral("openPeriodicTasks"));
    const auto refresh = [button, sender] {
        const auto count = sender->count();
        button->setText(tr("周期任务 · %1").arg(count));
        button->setVisible(count > 0);
    };
    QObject::connect(sender, &PeriodicSender::changed, button, refresh);
    QObject::connect(button, &QPushButton::clicked, sender, [button, sender] { openPeriodicTasks(sender, visibleWindow(button)); });
    refresh();
    return button;
}

} // namespace fhmi
