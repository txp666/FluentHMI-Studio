#include "designer/reference_impact_dialog.h"

#include "runtime/app_dialog.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonObject>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }
}

QString widgetLabel(const QJsonObject &widget)
{
    const auto id = widget.value(QStringLiteral("id")).toString();
    const auto props = widget.value(QStringLiteral("properties")).toObject();
    const auto title = props.value(QStringLiteral("title")).toString(
        props.value(QStringLiteral("text")).toString()).trimmed();
    return title.isEmpty() || title == id ? id : QStringLiteral("%1 · %2").arg(title, id);
}

bool confirmReferences(QWidget *parent, const QString &title, const QString &message,
                       const QList<ReferenceRow> &rows, const QString &continueText, const QString &cancelText,
                       const QString &objectName)
{
    AppDialog dialog(title, parent);
    dialog.setObjectName(objectName);
    const int visibleRows = int(std::min<qsizetype>(rows.size(), 10));
    dialog.setPreferredSize(QSize(760, std::max(340, 250 + 34 * visibleRows)));
    dialog.yesButton()->setText(continueText);
    dialog.cancelButton()->setText(cancelText);
    auto *summary = new FluentQt::BodyLabel(message, &dialog);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    dialog.viewLayout()->addWidget(summary);

    auto *table = new FluentQt::TableWidget(&dialog);
    table->setObjectName(objectName + QStringLiteral("References"));
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({tr("类别"), tr("字段或引用"), tr("变更或用途")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(34);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setRowCount(rows.size());
    for (qsizetype row = 0; row < rows.size(); ++row) {
        const auto &reference = rows.at(row);
        table->setItem(row, 0, new QTableWidgetItem(reference.category));
        table->setItem(row, 1, new QTableWidgetItem(reference.name));
        table->setItem(row, 2, new QTableWidgetItem(reference.detail));
    }
    dialog.viewLayout()->addWidget(table, 1);
    return dialog.exec() == QDialog::Accepted;
}

} // namespace fhmi
