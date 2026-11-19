#include "runtime/project_data_view.h"

#include "core/binding.h"
#include "core/project_logic.h"
#include "runtime/app_dialog.h"
#include "runtime/project_data.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <QtCore/QCoreApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }

void editParameterValue(ProjectData *data, const QString &name, QWidget *parent)
{
    AppDialog dialog(tr("修改工程参数 · %1").arg(name), parent);
    dialog.setPreferredSize(QSize(470, 250));
    auto *layout = new QFormLayout;
    dialog.viewLayout()->addLayout(layout);
    auto *value = new FluentQt::LineEdit(&dialog);
    value->setText(displayString(data->sample(name).value));
    auto *problem = new FluentQt::CaptionLabel(&dialog);
    layout->addRow(tr("当前值"), value);
    layout->addRow(problem);
    dialog.setValidator([&] {
        const bool updated = data->setParameter(name, value->text());
        problem->setText(updated ? QString() : tr("工程参数的值与类型不匹配"));
        return updated;
    });
    dialog.exec();
}
}

void openProjectDataMonitor(const QJsonObject &logic, ProjectData *data, QWidget *parent)
{
    auto *dialog = new AppDialog(tr("工程数据监视"), parent);
    dialog->setObjectName(QStringLiteral("projectDataMonitor"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setPreferredSize(QSize(1050, 580));
    dialog->hideCancelButton();
    dialog->yesButton()->setText(tr("关闭"));
    QObject::connect(data, &QObject::destroyed, dialog, &QDialog::reject);
    auto *table = new FluentQt::TableWidget(dialog);
    table->setObjectName(QStringLiteral("projectDataReadings"));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels({tr("数据点"), tr("最近有效值"), tr("单位"), tr("数据质量"), tr("来源"), tr("最近采样")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    const auto points = logic.value(QStringLiteral("points")).toArray();
    table->setRowCount(points.size());
    for (qsizetype row = 0; row < points.size(); ++row) {
        for (int column = 0; column < table->columnCount(); ++column)
            table->setItem(row, column, new QTableWidgetItem);
    }
    dialog->viewLayout()->addWidget(table, 1);
    auto *details = new FluentQt::CaptionLabel(dialog);
    details->setWordWrap(true);
    details->setTextFormat(Qt::PlainText);
    dialog->viewLayout()->addWidget(details);
    auto *edit = new FluentQt::PushButton(tr("修改工程参数…"), dialog);
    dialog->viewLayout()->addWidget(edit, 0, Qt::AlignLeft);
    const auto selection = [table, data, points, details, edit] {
        const int row = table->currentRow();
        const auto point = row >= 0 ? points.at(row).toObject() : QJsonObject{};
        const auto sample = data->sample(point.value(QStringLiteral("name")).toString());
        details->setText(sample.detail);
        details->setVisible(!sample.detail.isEmpty());
        edit->setEnabled(isProjectParameter(point));
    };
    const auto update = [table, data, points, selection](const Message &changes) {
        const bool dark = FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
        for (qsizetype row = 0; row < points.size(); ++row) {
            const auto point = points.at(row).toObject();
            const auto name = point.value(QStringLiteral("name")).toString();
            if (!changes.isEmpty() && !changes.contains(name))
                continue;
            const auto sample = data->sample(name);
            const auto formula = point.value(QStringLiteral("expression")).toString();
            auto binding = point.value(QStringLiteral("binding")).toObject();
            if (binding.value(QStringLiteral("sourceCardId")).toString().isEmpty() && !sample.source.connectionId.isEmpty())
                binding.insert(QStringLiteral("sourceCardId"), sample.source.connectionId);
            if (sample.quality != DataQuality::Ambiguous && !sample.source.endpoint.isEmpty() &&
                binding.value(QStringLiteral("sourceEndpoint")).toString().isEmpty())
                binding.insert(QStringLiteral("sourceEndpoint"), sample.source.endpoint);
            const auto source = isProjectParameter(point) ? tr("工程参数") : formula.isEmpty()
                ? dataPointSourceText(binding) : formula;
            const QStringList cells{name, sample.value.isValid() ? displayString(sample.value) : QString(),
                point.value(QStringLiteral("unit")).toString(), dataQualityText(sample.quality), source,
                sample.timestamp.isValid() ? sample.timestamp.toLocalTime().toString(QStringLiteral("HH:mm:ss.zzz")) : QString()};
            for (int column = 0; column < table->columnCount(); ++column)
                table->item(row, column)->setText(cells.at(column));
            auto *quality = table->item(row, 3);
            const bool warning = sample.quality != DataQuality::Current && sample.quality != DataQuality::Waiting;
            quality->setForeground(warning ? QBrush(dark ? QColor(0xFFB270) : QColor(0x93400D)) : QBrush());
        }
        selection();
    };
    QObject::connect(data, &ProjectData::valuesChanged, dialog, update);
    QObject::connect(table, &QTableWidget::itemSelectionChanged, dialog, selection);
    QObject::connect(edit, &QPushButton::clicked, dialog, [table, data, dialog] {
        editParameterValue(data, table->item(table->currentRow(), 0)->text(), dialog);
    });
    update({});
    dialog->open();
}
} // namespace fhmi
