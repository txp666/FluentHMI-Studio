#include "designer/project_logic_dialog.h"
#include "designer/project_frame_templates.h"
#include "core/binding.h"
#include "core/expression.h"
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
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QSignalBlocker>
#include <QtCore/QSet>
#include <QtCore/QUuid>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
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

QJsonValue scalarValue(const QString &text)
{
    if (text.trimmed() == QLatin1String("True"))
        return true;
    if (text.trimmed() == QLatin1String("False"))
        return false;
    const auto json = QJsonDocument::fromJson('[' + text.toUtf8() + ']');
    if (json.isArray() && json.array().size() == 1) {
        const auto value = json.array().first();
        if (value.isDouble() || value.isBool() || value.isString())
            return value;
    }
    return QJsonValue(text);
}

QString parameterType(const QJsonValue &value)
{
    return value.isDouble() ? tr("数值") : value.isBool() ? tr("布尔") : tr("文本");
}

bool editParameter(QJsonValue *initial, QWidget *parent)
{
    AppDialog dialog(tr("工程参数"), parent);
    dialog.setPreferredSize(QSize(450, 270));
    auto *form = new QFormLayout;
    dialog.viewLayout()->addLayout(form);
    auto *type = new FluentQt::ComboBox(&dialog);
    type->addItems({tr("数值"), tr("布尔"), tr("文本")});
    type->setCurrentIndex(initial->isDouble() ? 0 : initial->isBool() ? 1 : 2);
    auto *value = new FluentQt::LineEdit(&dialog);
    value->setText(initial->isString() ? initial->toString() : displayString(initial->toVariant()));
    form->addRow(tr("数据类型"), type);
    form->addRow(tr("初始值"), value);
    auto *problem = new FluentQt::CaptionLabel(&dialog);
    form->addRow(problem);
    const auto read = [type, value] {
        if (type->currentIndex() == 2)
            return QJsonValue(value->text());
        const auto parsed = scalarValue(value->text());
        if (type->currentIndex() == 0)
            return parsed.isDouble() ? parsed : QJsonValue();
        const auto boolean = bindingBoolean(value->text());
        return boolean ? QJsonValue(*boolean) : QJsonValue();
    };
    dialog.setValidator([&] {
        if (!read().isNull())
            return true;
        problem->setText(tr("初始值与所选类型不匹配"));
        return false;
    });
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *initial = read();
    return true;
}

QString thresholdCondition(const QStringList &names, QWidget *parent)
{
    AppDialog dialog(tr("按阈值生成条件"), parent);
    dialog.setPreferredSize(QSize(500, 310));
    auto *form = new QFormLayout;
    dialog.viewLayout()->addLayout(form);
    auto *point = new FluentQt::ComboBox(&dialog);
    point->addItems(names);
    point->setCurrentIndex(0);
    auto *comparison = new FluentQt::ComboBox(&dialog);
    comparison->addItems({QStringLiteral(">"), QStringLiteral(">="), QStringLiteral("<"), QStringLiteral("<="),
                          QStringLiteral("=="), QStringLiteral("!=")});
    comparison->setCurrentIndex(0);
    auto *threshold = new FluentQt::LineEdit(&dialog);
    threshold->setText(QStringLiteral("0"));
    form->addRow(tr("工程数据点"), point);
    form->addRow(tr("比较运算"), comparison);
    form->addRow(tr("阈值"), threshold);
    auto *problem = new FluentQt::CaptionLabel(&dialog);
    form->addRow(problem);
    dialog.setValidator([&] {
        const auto value = scalarValue(threshold->text());
        if (!point->currentText().isEmpty() && (value.isDouble() || value.isBool()))
            return true;
        problem->setText(tr("请选择数据点并填写数值或布尔阈值"));
        return false;
    });
    if (dialog.exec() != QDialog::Accepted)
        return {};
    const auto reference = Expression::renameReference(QStringLiteral("T[\"_\"]"), QStringLiteral("_"), point->currentText());
    return QStringLiteral("%1 %2 %3").arg(reference, comparison->currentText(), displayString(scalarValue(threshold->text()).toVariant()));
}

void testCondition(const QJsonObject &logic, const QString &expression, QWidget *parent)
{
    AppDialog dialog(tr("表达式试算"), parent);
    dialog.setPreferredSize(QSize(570, 425));
    dialog.hideCancelButton();
    dialog.yesButton()->setText(tr("关闭"));
    auto *formula = new FluentQt::BodyLabel(expression, &dialog);
    formula->setTextFormat(Qt::PlainText);
    formula->setWordWrap(true);
    dialog.viewLayout()->addWidget(formula);
    auto *table = new FluentQt::TableWidget(&dialog);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({tr("工程数据点"), tr("试算输入")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    const auto names = Expression::references(expression);
    table->setRowCount(names.size());
    for (qsizetype row = 0; row < names.size(); ++row) {
        auto *name = item(names.at(row));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 0, name);
        const auto initial = findDataPoint(logic, name->text()).value(QStringLiteral("initialValue"));
        table->setItem(row, 1, item(initial.isUndefined() ? QStringLiteral("0") : displayString(initial.toVariant())));
    }
    dialog.viewLayout()->addWidget(table, 1);
    auto *result = new FluentQt::BodyLabel(&dialog);
    dialog.viewLayout()->addWidget(result);
    const auto evaluate = [&] {
        const auto value = Expression::evaluate(expression, {}, [&](const QString &name) {
            return scalarValue(table->item(names.indexOf(name), 1)->text()).toVariant();
        });
        result->setText(value.isValid() ? tr("试算结果 %1").arg(displayString(value)) : tr("表达式无法得出有效结果"));
    };
    QObject::connect(table, &QTableWidget::itemChanged, &dialog, evaluate);
    evaluate();
    dialog.exec();
}
QString devicePointSummary(const QJsonObject &point)
{
    auto summary = dataPointSourceText(point.value(QStringLiteral("binding")).toObject());
    if (const int age = point.value(QStringLiteral("staleAfterMs")).toInt(); age > 0)
        summary += tr(" · %1 ms 后过期").arg(age);
    return summary;
}

} // namespace

bool editDevicePoint(Project *project, QJsonObject *point, QWidget *parent, bool editName)
{
    Project reference = *project;
    AppDialog dialog(tr("数据点来源"), parent);
    dialog.setObjectName(QStringLiteral("projectDataSourceDialog"));
    dialog.setPreferredSize(QSize(565, 670));
    CanvasItem source(QStringLiteral("NumericDisplay"), {}, QStringLiteral("data-point"));
    const auto binding = point->value(QStringLiteral("binding")).toObject();
    source.setBinding(binding.isEmpty() ? QJsonObject{{QStringLiteral("parseMode"), ParseMode::KeyValue}} : binding);
    auto *form = new QFormLayout;
    dialog.viewLayout()->addLayout(form);
    auto *name = new FluentQt::LineEdit(&dialog);
    name->setText(point->value(QStringLiteral("name")).toString());
    if (editName)
        form->addRow(tr("名称"), name);
    else
        name->hide();
    auto *period = new FluentQt::SpinBox(&dialog);
    period->setRange(0, 86400000);
    period->setSpecialValueText(tr("不限"));
    period->setValue(point->value(QStringLiteral("staleAfterMs")).toInt());
    form->addRow(tr("数据有效时间(ms)"), period);
    auto *editor = new PropertyEditor(&dialog);
    editor->setProject(&reference);
    editor->showBinding(&source);
    const auto originalPoints = reference.logic.value(QStringLiteral("points")).toArray();
    qsizetype pointRow = originalPoints.size();
    if (!editName) {
        for (qsizetype row = 0; row < originalPoints.size(); ++row) {
            if (originalPoints.at(row).toObject().value(QStringLiteral("name")).toString() == name->text()) {
                pointRow = row;
                break;
            }
        }
    }
    QJsonObject lastBinding;
    const auto syncPointDraft = [&] {
        auto points = reference.logic.value(QStringLiteral("points")).toArray();
        auto definition = pointRow < points.size() ? points.at(pointRow).toObject() : *point;
        definition.remove(QStringLiteral("expression"));
        definition.remove(QStringLiteral("initialValue"));
        definition.insert(QStringLiteral("name"), name->text().trimmed());
        lastBinding = source.binding();
        definition.insert(QStringLiteral("binding"), lastBinding);
        if (pointRow < points.size())
            points[pointRow] = definition;
        else
            points.append(definition);
        reference.logic.insert(QStringLiteral("points"), points);
    };
    QObject::connect(name, &QLineEdit::textChanged, &dialog, syncPointDraft);
    QObject::connect(editor, &PropertyEditor::modified, &dialog, syncPointDraft);
    QObject::connect(editor, &PropertyEditor::projectSettingsChanged, &dialog, [&] {
        const auto stored = reference.logic.value(QStringLiteral("points")).toArray().at(pointRow).toObject()
            .value(QStringLiteral("binding")).toObject();
        auto current = source.binding();
        // Apply migrated references alongside edits to other properties of this data point.
        for (auto it = stored.cbegin(); it != stored.cend(); ++it)
            if (it.value() != lastBinding.value(it.key()) && current.value(it.key()) == lastBinding.value(it.key()))
                current.insert(it.key(), it.value());
        source.setBinding(current);
        syncPointDraft();
    });
    syncPointDraft();
    dialog.viewLayout()->addWidget(editor, 1);
    auto *problem = new FluentQt::CaptionLabel(&dialog);
    problem->setWordWrap(true);
    dialog.viewLayout()->addWidget(problem);
    dialog.setValidator([&] {
        const QString pointName = name->text().trimmed();
        const auto names = dataPointNames(project->logic);
        const auto error = editName && (pointName.isEmpty() || names.contains(pointName))
            ? tr("数据点名称不能为空或重复") : dataPointSourceProblem(source.binding(), reference.widgets, reference.parserType());
        problem->setText(error);
        return error.isEmpty();
    });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    if (accepted) {
        auto points = reference.logic.value(QStringLiteral("points")).toArray();
        if (pointRow == originalPoints.size())
            points.removeAt(pointRow);
        if (project->logic.contains(QStringLiteral("points")))
            project->logic.insert(QStringLiteral("points"), points);
        project->serial = reference.serial;
        project->widgets = reference.widgets;
        project->frameTemplates = reference.frameTemplates;
        point->insert(QStringLiteral("name"), name->text().trimmed());
        point->insert(QStringLiteral("binding"), source.binding());
        if (period->value() > 0)
            point->insert(QStringLiteral("staleAfterMs"), period->value());
        else
            point->remove(QStringLiteral("staleAfterMs"));
    }
    editor->showItem(nullptr);
    return accepted;
}

bool editProjectLogic(Project *project, QWidget *parent)
{
    Project draft = *project;
    AppDialog dialog(tr("工程数据与报警"), parent);
    dialog.setObjectName(QStringLiteral("projectLogicDialog"));
    dialog.setPreferredSize(QSize(900, 660));
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
    tabs->addItem(QStringLiteral("frames"), tr("帧格式模板"));
    layout->addWidget(tabs, 0, Qt::AlignLeft);
    auto *pages = new QStackedWidget(&dialog);
    layout->addWidget(pages, 1);
    auto *pointsPage = new QWidget(pages);
    auto *alarmsPage = new QWidget(pages);
    pages->addWidget(pointsPage);
    pages->addWidget(alarmsPage);
    auto *points = new FluentQt::TableWidget(pointsPage);
    points->setObjectName(QStringLiteral("projectDataPoints"));
    points->setColumnCount(5);
    points->setHorizontalHeaderLabels({tr("名称"), tr("取值方式"), tr("来源 / 表达式"), tr("单位"), tr("配置")});
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
    auto *sound = new FluentQt::CheckBox(tr("活动报警声音提醒（确认或消音后停止）"), &dialog);
    sound->setChecked(project->logic.value(QStringLiteral("soundEnabled")).toBool());
    const auto readLogic = [&] {
        QJsonArray pointDefs, alarmDefs;
        for (int row = 0; row < points->rowCount(); ++row) {
            QJsonObject p{{QStringLiteral("name"), points->item(row, 0)->text().trimmed()}};
            const auto stored = points->item(row, 2)->data(Qt::UserRole).toJsonObject();
            const auto unit = points->item(row, 3)->text().trimmed();
            if (!unit.isEmpty())
                p.insert(QStringLiteral("unit"), unit);
            const int mode = static_cast<FluentQt::ComboBox *>(points->cellWidget(row, 1))->currentIndex();
            if (mode == 1) {
                p.insert(QStringLiteral("expression"), points->item(row, 2)->text().trimmed());
            } else if (mode == 2) {
                const auto initial = stored.value(QStringLiteral("initialValue"));
                p.insert(QStringLiteral("initialValue"), initial.isUndefined() ? QJsonValue(0) : initial);
            } else {
                p.insert(QStringLiteral("binding"), stored.value(QStringLiteral("binding")).toObject());
                if (stored.contains(QStringLiteral("staleAfterMs")))
                    p.insert(QStringLiteral("staleAfterMs"), stored.value(QStringLiteral("staleAfterMs")));
            }
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
    const auto syncPointBindings = [&] {
        const auto updated = draft.logic.value(QStringLiteral("points")).toArray();
        const QSignalBlocker blocker(points);
        for (int row = 0; row < points->rowCount(); ++row) {
            const auto binding = updated.at(row).toObject().value(QStringLiteral("binding"));
            if (!binding.isObject())
                continue;
            auto *definition = points->item(row, 2);
            auto stored = definition->data(Qt::UserRole).toJsonObject();
            if (binding != stored.value(QStringLiteral("binding"))) {
                stored.insert(QStringLiteral("binding"), binding);
                definition->setData(Qt::UserRole, stored);
                definition->setText(devicePointSummary(stored));
            }
        }
    };
    const auto addPoint = [&](const QJsonObject &point) {
        const int row = points->rowCount();
        points->insertRow(row);
        auto *name = item(point.value(QStringLiteral("name")).toString());
        name->setData(Qt::UserRole, name->text());
        points->setItem(row, 0, name);
        auto *mode = new FluentQt::ComboBox(points);
        mode->addItems({tr("设备数据"), tr("计算表达式"), tr("工程参数")});
        const auto expression = point.value(QStringLiteral("expression")).toString();
        mode->setCurrentIndex(isProjectParameter(point) ? 2 : expression.isEmpty() ? 0 : 1);
        points->setCellWidget(row, 1, mode);
        auto *definition = item(expression);
        definition->setData(Qt::UserRole, point);
        points->setItem(row, 2, definition);
        points->setItem(row, 3, item(point.value(QStringLiteral("unit")).toString()));
        auto *actions = new QWidget(points);
        auto *rowLayout = new QHBoxLayout(actions);
        rowLayout->setContentsMargins(3, 1, 3, 1);
        auto *configure = new FluentQt::PushButton(tr("设置绑定…"), actions);
        rowLayout->addWidget(configure);
        points->setCellWidget(row, 4, actions);
        const auto refresh = [mode, definition, configure] {
            const bool formula = mode->currentIndex() == 1;
            const bool parameter = mode->currentIndex() == 2;
            definition->setFlags(formula ? definition->flags() | Qt::ItemIsEditable
                                         : definition->flags() & ~Qt::ItemIsEditable);
            configure->setVisible(!formula);
            configure->setText(parameter ? tr("设置初始值…") : tr("设置绑定…"));
            const auto data = definition->data(Qt::UserRole).toJsonObject();
            if (parameter) {
                const auto initial = data.value(QStringLiteral("initialValue"));
                const auto value = initial.isUndefined() ? QJsonValue(0) : initial;
                definition->setText(QStringLiteral("%1 · %2").arg(parameterType(value), displayString(value.toVariant())));
            } else if (!formula) {
                definition->setText(devicePointSummary(data));
            }
        };
        QObject::connect(mode, &FluentQt::ComboBox::currentIndexChanged, &dialog, [definition, refresh] {
            definition->setText({});
            refresh();
        });
        QObject::connect(configure, &QPushButton::clicked, &dialog, [&, definition, mode, refresh] {
            auto data = definition->data(Qt::UserRole).toJsonObject();
            if (mode->currentIndex() == 2) {
                auto initial = data.value(QStringLiteral("initialValue"));
                if (initial.isUndefined())
                    initial = 0;
                if (!editParameter(&initial, &dialog))
                    return;
                data.insert(QStringLiteral("initialValue"), initial);
            } else {
                draft.logic = readLogic();
                data.insert(QStringLiteral("name"), points->item(definition->row(), 0)->text().trimmed());
                if (!editDevicePoint(&draft, &data, &dialog, false))
                    return;
                syncPointBindings();
            }
            definition->setData(Qt::UserRole, data);
            refresh();
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
        QObject::connect(plus, &QPushButton::clicked, &dialog, [add, table] {
            add();
            table->setCurrentCell(table->rowCount() - 1, 0);
            table->scrollToBottom();
        });
        QObject::connect(minus, &QPushButton::clicked, &dialog, [table] {
            if (table->currentRow() >= 0)
                table->removeRow(table->currentRow());
        });
        return bar;
    };
    auto *pointActions = page(pointsPage, points,
         [&] { addPoint({{QStringLiteral("name"), nextName(points, QStringLiteral("Point"))}}); });
    auto *alarmActions = page(alarmsPage, alarms,
         [&] { addAlarm({{QStringLiteral("id"), nextName(alarms, QStringLiteral("Alarm"))}}); });
    alarmsPage->layout()->addWidget(sound);
    auto *framesPage = createFrameTemplateLibrary(&draft, pages, {QStringLiteral("hex"), {}}, {}, syncPointBindings);
    pages->addWidget(framesPage);
    QObject::connect(tabs, &FluentQt::SegmentedWidget::currentItemChanged, pages, [&, framesPage](const QString &key) {
        if (key == QLatin1String("frames"))
            draft.logic = readLogic();
        pages->setCurrentWidget(key == QLatin1String("frames") ? framesPage
            : key == QLatin1String("alarms") ? alarmsPage : pointsPage);
    });
    tabs->setCurrentItem(QStringLiteral("points"));
    auto *problem = new FluentQt::BodyLabel(&dialog);
    problem->setWordWrap(true);
    layout->addWidget(problem);
    const auto action = [&dialog](QHBoxLayout *bar, QTableWidget *table, const QString &label, const auto &perform) {
        auto *button = new FluentQt::PushButton(label, &dialog);
        bar->insertWidget(bar->count() - 1, button);
        button->setEnabled(table->currentRow() >= 0);
        QObject::connect(table, &QTableWidget::itemSelectionChanged, button, [button, table] {
            button->setEnabled(table->currentRow() >= 0 && !table->selectedItems().isEmpty());
        });
        QObject::connect(button, &QPushButton::clicked, &dialog, [table, perform] { perform(table->currentRow()); });
    };
    action(pointActions, points, tr("查看引用"), [&](int row) {
        draft.logic = readLogic();
        const auto name = points->item(row, 0)->text();
        const auto found = dataPointReferences(draft, name);
        AppDialog references(tr("%1 的引用").arg(name), &dialog);
        references.setPreferredSize(QSize(525, 370));
        references.hideCancelButton();
        references.yesButton()->setText(tr("关闭"));
        auto *list = new FluentQt::ListWidget(&references);
        list->addItems(found.isEmpty() ? QStringList{tr("暂无引用")} : found);
        references.viewLayout()->addWidget(list, 1);
        references.exec();
    });
    action(alarmActions, alarms, tr("按阈值生成条件"), [&](int row) {
        const auto text = thresholdCondition(dataPointNames(readLogic()), &dialog);
        if (!text.isEmpty())
            alarms->item(row, 1)->setText(text);
    });
    action(alarmActions, alarms, tr("表达式试算"), [&](int row) {
        testCondition(readLogic(), alarms->item(row, 1)->text(), &dialog);
    });
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
    project->serial = draft.serial;
    project->widgets = draft.widgets;
    project->frameTemplates = draft.frameTemplates;
    return true;
}
} // namespace fhmi
