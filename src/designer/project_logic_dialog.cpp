#include "designer/project_logic_dialog.h"
#include "core/project_logic.h"
#include "designer/canvas_item.h"
#include "designer/property_editor.h"
#include "runtime/app_dialog.h"
#include "runtime/field_help.h"
#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QSignalBlocker>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }
QTableWidgetItem *item(const QString &text) { return new QTableWidgetItem(text); }
QString nextName(QTableWidget *table, const QString &prefix)
{
    QSet<QString> used;
    for (int row = 0; row < table->rowCount(); ++row)
        used.insert(table->item(row, 0)->text().trimmed());
    int suffix = table->rowCount() + 1;
    while (used.contains(prefix + QString::number(suffix)))
        ++suffix;
    return prefix + QString::number(suffix);
}
} // namespace
bool editProjectLogic(Project *project, QWidget *parent)
{
    Project draft = *project;
    AppDialog dialog(tr("工程数据与报警"), parent);
    dialog.setObjectName(QStringLiteral("projectLogicDialog"));
    dialog.setPreferredSize(QSize(900, 620));
    auto *layout = dialog.viewLayout();
    auto *top = new QHBoxLayout;
    auto *intro = new FluentQt::BodyLabel(tr("定义共用的数据点，再用表达式汇总状态或触发报警。"), &dialog);
    intro->setWordWrap(true);
    top->addWidget(intro, 1);
    top->addWidget(createFieldHelpButton(FieldHelpTopic::ProjectLogic, &dialog));
    layout->addLayout(top);
    auto *tabs = new FluentQt::SegmentedWidget(&dialog);
    tabs->setObjectName(QStringLiteral("projectLogicTabs"));
    tabs->addItem(QStringLiteral("points"), tr("数据点"));
    tabs->addItem(QStringLiteral("alarms"), tr("报警"));
    layout->addWidget(tabs, 0, Qt::AlignLeft);
    auto *pages = new QStackedWidget(&dialog);
    layout->addWidget(pages, 1);
    auto *pointsPage = new QWidget(pages);
    auto *alarmsPage = new QWidget(pages);
    pages->addWidget(pointsPage);
    pages->addWidget(alarmsPage);
    QObject::connect(tabs, &FluentQt::SegmentedWidget::currentItemChanged, pages,
                     [pages](const QString &key) { pages->setCurrentIndex(key == QLatin1String("alarms") ? 1 : 0); });
    tabs->setCurrentItem(QStringLiteral("points"));
    auto *points = new FluentQt::TableWidget(pointsPage);
    points->setObjectName(QStringLiteral("projectDataPoints"));
    points->setColumnCount(4);
    points->setHorizontalHeaderLabels({tr("名称"), tr("取值方式"), tr("来源 / 表达式"), tr("来源设置")});
    auto *alarms = new FluentQt::TableWidget(alarmsPage);
    alarms->setObjectName(QStringLiteral("projectAlarms"));
    alarms->setColumnCount(5);
    alarms->setHorizontalHeaderLabels({tr("名称"), tr("触发条件"), tr("级别"), tr("需要确认"), tr("报警文字")});
    for (auto *table : {points, alarms}) {
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(38);
    }
    points->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    alarms->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    alarms->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    const auto addPoint = [&](const QJsonObject &point) {
        const int row = points->rowCount();
        points->insertRow(row);
        auto *name = item(point.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole, name->text());
        points->setItem(row, 0, name);
        auto *mode = new FluentQt::ComboBox(points);
        mode->addItems({tr("接收数据"), tr("计算表达式")});
        const auto expression = point.value(QStringLiteral("expression")).toString();
        mode->setCurrentIndex(expression.isEmpty() ? 0 : 1);
        points->setCellWidget(row, 1, mode);
        auto *definition = item(expression);
        definition->setData(Qt::UserRole, point.value(QStringLiteral("binding")).toObject());
        points->setItem(row, 2, definition);
        auto *configure = new FluentQt::PushButton(tr("设置绑定…"), points);
        points->setCellWidget(row, 3, configure);
        const auto refresh = [mode, definition, configure] {
            const bool formula = mode->currentIndex() == 1;
            definition->setFlags(formula ? definition->flags() | Qt::ItemIsEditable
                                         : definition->flags() & ~Qt::ItemIsEditable);
            configure->setEnabled(!formula);
            if (!formula) {
                const auto b = definition->data(Qt::UserRole).toJsonObject();
                definition->setText(b.isEmpty()
                                        ? tr("未配置")
                                        : b.value(QStringLiteral("sourceCardId")).toString() + QStringLiteral(" · ") +
                                              b.value(QStringLiteral("parseMode")).toString() + QStringLiteral(" · ") +
                                              b.value(QStringLiteral("key")).toString());
            }
        };
        QObject::connect(mode, &FluentQt::ComboBox::currentIndexChanged, &dialog, [definition, refresh] {
            definition->setText({});
            refresh();
        });
        QObject::connect(configure, &QPushButton::clicked, &dialog, [&, definition, refresh] {
            AppDialog sourceDialog(tr("数据点来源"), &dialog);
            sourceDialog.setObjectName(QStringLiteral("projectDataSourceDialog"));
            sourceDialog.setPreferredSize(QSize(520, 620));
            CanvasItem source(QStringLiteral("NumericDisplay"), {}, QStringLiteral("data-point"));
            source.setBinding(definition->data(Qt::UserRole).toJsonObject());
            auto *editor = new PropertyEditor(&sourceDialog);
            editor->setProject(&draft);
            editor->showBinding(&source);
            sourceDialog.viewLayout()->addWidget(editor, 1);
            if (sourceDialog.exec() == QDialog::Accepted) {
                definition->setData(Qt::UserRole, source.binding());
                refresh();
            }
            editor->showItem(nullptr);
        });
        refresh();
    };
    const auto addAlarm = [&](const QJsonObject &alarm) {
        const int row = alarms->rowCount();
        alarms->insertRow(row);
        alarms->setItem(row, 0, item(alarm.value(QStringLiteral("id")).toString()));
        alarms->setItem(row, 1, item(alarm.value(QStringLiteral("condition")).toString()));
        auto *level = new FluentQt::SpinBox(alarms);
        level->setRange(1, 3);
        level->setValue(alarm.value(QStringLiteral("level")).toInt(2));
        alarms->setCellWidget(row, 2, level);
        auto *ack = new FluentQt::CheckBox(alarms);
        ack->setChecked(alarm.value(QStringLiteral("ackRequired")).toBool(true));
        alarms->setCellWidget(row, 3, ack);
        alarms->setItem(row, 4, item(alarm.value(QStringLiteral("text")).toString()));
    };
    for (const auto &p : project->logic.value(QStringLiteral("points")).toArray())
        addPoint(p.toObject());
    for (const auto &a : project->logic.value(QStringLiteral("alarms")).toArray())
        addAlarm(a.toObject());
    const auto page = [&](QWidget *host, QTableWidget *table, const std::function<void()> &add) {
        auto *box = new QVBoxLayout(host);
        box->addWidget(table, 1);
        auto *bar = new QHBoxLayout;
        box->addLayout(bar);
        auto *plus = new FluentQt::PushButton(tr("添加"), host);
        auto *minus = new FluentQt::PushButton(tr("删除所选"), host);
        bar->addWidget(plus);
        bar->addWidget(minus);
        bar->addStretch();
        QObject::connect(plus, &QPushButton::clicked, &dialog, add);
        QObject::connect(minus, &QPushButton::clicked, &dialog, [table] {
            if (table->currentRow() >= 0)
                table->removeRow(table->currentRow());
        });
    };
    page(pointsPage, points,
         [&] { addPoint({{QStringLiteral("name"), nextName(points, QStringLiteral("Point"))}}); });
    page(alarmsPage, alarms,
         [&] { addAlarm({{QStringLiteral("id"), nextName(alarms, QStringLiteral("Alarm"))}}); });
    auto *sound = new FluentQt::CheckBox(tr("活动报警声音提醒（确认或消音后停止）"), &dialog);
    sound->setChecked(project->logic.value(QStringLiteral("soundEnabled")).toBool());
    layout->addWidget(sound);
    auto *problem = new FluentQt::BodyLabel(&dialog);
    problem->setWordWrap(true);
    layout->addWidget(problem);
    const auto readLogic = [&] {
        QJsonArray pointDefs, alarmDefs;
        for (int row = 0; row < points->rowCount(); ++row) {
            QJsonObject p{{QStringLiteral("name"), points->item(row, 0)->text().trimmed()}};
            if (static_cast<FluentQt::ComboBox *>(points->cellWidget(row, 1))->currentIndex() == 1)
                p.insert(QStringLiteral("expression"), points->item(row, 2)->text().trimmed());
            else
                p.insert(QStringLiteral("binding"), points->item(row, 2)->data(Qt::UserRole).toJsonObject());
            pointDefs.append(p);
        }
        for (int row = 0; row < alarms->rowCount(); ++row)
            alarmDefs.append(QJsonObject{
                {QStringLiteral("id"), alarms->item(row, 0)->text().trimmed()},
                {QStringLiteral("condition"), alarms->item(row, 1)->text().trimmed()},
                {QStringLiteral("level"), static_cast<QSpinBox *>(alarms->cellWidget(row, 2))->value()},
                {QStringLiteral("ackRequired"), static_cast<QCheckBox *>(alarms->cellWidget(row, 3))->isChecked()},
                {QStringLiteral("text"), alarms->item(row, 4)->text().trimmed()}});
        return QJsonObject{
            {QStringLiteral("points"), pointDefs},
            {QStringLiteral("alarms"), alarmDefs},
            {QStringLiteral("soundEnabled"), sound->isChecked()},
            {QStringLiteral("id"),
             project->logic.value(QStringLiteral("id")).toString(QUuid::createUuid().toString(QUuid::WithoutBraces))}};
    };
    QObject::connect(points, &QTableWidget::itemChanged, &dialog, [&](QTableWidgetItem *name) {
        if (name->column() != 0)
            return;
        const QString previous = name->data(Qt::UserRole).toString();
        const QString current = name->text().trimmed();
        if (previous == current)
            return;
        const QSignalBlocker pointBlock(points), alarmBlock(alarms);
        bool duplicate = false;
        for (int row = 0; row < points->rowCount(); ++row)
            duplicate |= row != name->row() && points->item(row, 0)->text().trimmed() == current;
        if (current.isEmpty() || duplicate) {
            name->setText(previous);
            problem->setText(tr("数据点名称不能为空或重复"));
            return;
        }
        name->setText(current);
        name->setData(Qt::UserRole, current);
        draft.logic = readLogic();
        renameDataPoint(&draft, previous, current);
        const auto renamedPoints = draft.logic.value(QStringLiteral("points")).toArray();
        for (int row = 0; row < points->rowCount(); ++row)
            if (static_cast<FluentQt::ComboBox *>(points->cellWidget(row, 1))->currentIndex() == 1)
                points->item(row, 2)->setText(renamedPoints[row].toObject().value(QStringLiteral("expression")).toString());
        const auto renamedAlarms = draft.logic.value(QStringLiteral("alarms")).toArray();
        for (int row = 0; row < alarms->rowCount(); ++row)
            alarms->item(row, 1)->setText(renamedAlarms[row].toObject().value(QStringLiteral("condition")).toString());
        problem->clear();
    });
    dialog.setValidator([&] {
        draft.logic = readLogic();
        if (points->rowCount() == 0 && alarms->rowCount() == 0)
            draft.logic = {};
        QString error;
        if (!Project::validate(draft.toJson(), &error)) {
            problem->setText(error);
            return false;
        }
        return true;
    });
    if (dialog.exec() != QDialog::Accepted)
        return false;
    project->logic = draft.logic;
    project->widgets = draft.widgets;
    return true;
}
} // namespace fhmi
