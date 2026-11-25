#include "runtime/project_data_view.h"

#include "core/binding.h"
#include "core/project_logic.h"
#include "runtime/app_dialog.h"
#include "runtime/project_data.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Window/FluentTitleBar.h>
#include <FluentQtWidgets/Window/FluentWindow.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {
namespace {
QString tr(const char *s) { return QCoreApplication::translate("fhmi", s); }

constexpr int kRefreshIntervalMs = 150;

QHash<ProjectData *, QPointer<QWidget>> &openMonitors()
{
    static QHash<ProjectData *, QPointer<QWidget>> monitors;
    return monitors;
}

class HostCloseWatcher : public QObject
{
  public:
    HostCloseWatcher(QWidget *host, QWidget *target) : QObject(target), m_host(host), m_target(target)
    {
        if (m_host) {
            m_host->installEventFilter(this);
            connect(m_host, &QObject::destroyed, target, &QWidget::close);
        }
    }

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_host && event->type() == QEvent::Close && m_target) {
            m_target->close();
        }
        return false;
    }

  private:
    QPointer<QWidget> m_host;
    QPointer<QWidget> m_target;
};

struct RowMeta
{
    QString name;
    QString unit;
    QString formula;
    QJsonObject baseBinding;
    bool isParameter = false;
    bool lastWarning = false;
    bool warningInitialized = false;
};

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
} // namespace

void openProjectDataMonitor(const QJsonObject &logic, ProjectData *data, QWidget *parent)
{
    if (!data) {
        return;
    }
    auto &monitors = openMonitors();
    if (QWidget *existing = monitors.value(data)) {
        if (existing->isMinimized()) {
            existing->showNormal();
        }
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }

    // Use an independent top-level window rather than a translucent MaskDialogBase child overlay:
    // in exported apps (PlayerWindow), a translucent child overlay over QGraphicsView forces full
    // scaled canvas + 30-layer shadow recompositing on every data update and causes severe lag/freezes.
    QWidget *host = visibleWindow(parent);
    auto *window = new FluentQt::FluentWidget(nullptr);
    window->setObjectName(QStringLiteral("projectDataMonitor"));
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setMicaEffectEnabled(FluentQt::FluentConfig::instance()->isMicaEnabled());
    window->setWindowTitle(tr("工程数据监视"));
    window->setWindowIcon(host && !host->windowIcon().isNull() ? host->windowIcon()
                                                               : QIcon(QStringLiteral(":/fluenthmi/logo.png")));
    window->titleBar()->raise();
    monitors.insert(data, window);
    QObject::connect(window, &QObject::destroyed, data, [data]() { openMonitors().remove(data); });
    QObject::connect(data, &QObject::destroyed, window, &QWidget::close);
    if (host) {
        new HostCloseWatcher(host, window);
    }

    auto *root = new QVBoxLayout(window);
    root->setContentsMargins(20, window->titleBar()->height() + 12, 20, 20);
    root->setSpacing(12);

    auto *table = new FluentQt::TableWidget(window);
    table->setObjectName(QStringLiteral("projectDataReadings"));
    enableHorizontalWheelScroll(table);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels({tr("数据点"), tr("最近有效值"), tr("单位"), tr("数据质量"), tr("来源"), tr("最近采样")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);

    const auto points = logic.value(QStringLiteral("points")).toArray();
    auto rows = std::make_shared<QList<RowMeta>>();
    auto rowByName = std::make_shared<QHash<QString, int>>();
    rows->reserve(points.size());
    table->setRowCount(points.size());
    for (qsizetype row = 0; row < points.size(); ++row) {
        const QJsonObject point = points.at(row).toObject();
        RowMeta meta;
        meta.name = point.value(QStringLiteral("name")).toString();
        meta.unit = point.value(QStringLiteral("unit")).toString();
        meta.formula = point.value(QStringLiteral("expression")).toString();
        meta.baseBinding = point.value(QStringLiteral("binding")).toObject();
        meta.isParameter = isProjectParameter(point);
        rowByName->insert(meta.name, int(row));
        rows->append(std::move(meta));
        for (int column = 0; column < table->columnCount(); ++column) {
            table->setItem(int(row), column, new QTableWidgetItem);
        }
    }
    root->addWidget(table, 1);

    auto *details = new FluentQt::CaptionLabel(window);
    details->setWordWrap(true);
    details->setTextFormat(Qt::PlainText);
    details->hide();
    root->addWidget(details);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);
    auto *edit = new FluentQt::PushButton(tr("修改工程参数…"), window);
    edit->setEnabled(false);
    auto *closeBtn = new FluentQt::PrimaryPushButton(tr("关闭"), window);
    closeBtn->setObjectName(QStringLiteral("appDialogAccept"));
    footer->addWidget(edit);
    footer->addStretch(1);
    footer->addWidget(closeBtn);
    root->addLayout(footer);

    const auto selection = [table, data, rows, details, edit]() {
        const int row = table->currentRow();
        if (row < 0 || row >= rows->size()) {
            details->clear();
            details->hide();
            edit->setEnabled(false);
            return;
        }
        const RowMeta &meta = rows->at(row);
        const auto sample = data->sample(meta.name);
        if (details->text() != sample.detail) {
            details->setText(sample.detail);
        }
        details->setVisible(!sample.detail.isEmpty());
        edit->setEnabled(meta.isParameter);
    };

    const auto applyRow = [table, data, rows](int row, bool dark) {
        RowMeta &meta = (*rows)[row];
        const auto sample = data->sample(meta.name);
        QString source;
        if (meta.isParameter) {
            source = tr("工程参数");
        } else if (!meta.formula.isEmpty()) {
            source = meta.formula;
        } else {
            QJsonObject binding = meta.baseBinding;
            if (binding.value(QStringLiteral("sourceCardId")).toString().isEmpty() &&
                !sample.source.connectionId.isEmpty()) {
                binding.insert(QStringLiteral("sourceCardId"), sample.source.connectionId);
            }
            if (sample.quality != DataQuality::Ambiguous && !sample.source.endpoint.isEmpty() &&
                binding.value(QStringLiteral("sourceEndpoint")).toString().isEmpty()) {
                binding.insert(QStringLiteral("sourceEndpoint"), sample.source.endpoint);
            }
            source = dataPointSourceText(binding);
        }
        const QString cells[6] = {
            meta.name,
            sample.value.isValid() ? displayString(sample.value) : QString(),
            meta.unit,
            dataQualityText(sample.quality),
            source,
            sample.timestamp.isValid() ? sample.timestamp.toLocalTime().toString(QStringLiteral("HH:mm:ss.zzz"))
                                       : QString()};
        for (int column = 0; column < 6; ++column) {
            if (auto *item = table->item(row, column); item && item->text() != cells[column]) {
                item->setText(cells[column]);
            }
        }
        const bool warning = sample.quality != DataQuality::Current && sample.quality != DataQuality::Waiting;
        if (!meta.warningInitialized || meta.lastWarning != warning) {
            meta.warningInitialized = true;
            meta.lastWarning = warning;
            if (auto *quality = table->item(row, 3)) {
                quality->setForeground(warning ? QBrush(dark ? QColor(0xFFB270) : QColor(0x93400D)) : QBrush());
            }
        }
    };

    auto dirty = std::make_shared<QSet<QString>>();
    auto *flushTimer = new QTimer(window);
    flushTimer->setSingleShot(true);
    flushTimer->setInterval(kRefreshIntervalMs);

    const auto flush = [table, rows, rowByName, dirty, applyRow, selection]() {
        if (dirty->isEmpty()) {
            return;
        }
        const bool dark = FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
        const int currentRow = table->currentRow();
        bool currentTouched = false;
        const QSet<QString> pending = std::exchange(*dirty, {});
        for (const QString &name : pending) {
            const auto it = rowByName->constFind(name);
            if (it == rowByName->cend()) {
                continue;
            }
            applyRow(it.value(), dark);
            if (it.value() == currentRow) {
                currentTouched = true;
            }
        }
        if (currentTouched) {
            selection();
        }
    };

    QObject::connect(flushTimer, &QTimer::timeout, window, flush);
    QObject::connect(data, &ProjectData::valuesChanged, window, [dirty, flushTimer](const Message &changes) {
        for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
            dirty->insert(it.key());
        }
        if (!dirty->isEmpty() && !flushTimer->isActive()) {
            flushTimer->start();
        }
    });
    QObject::connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, window,
                     [rows, applyRow](FluentQt::Theme) {
                         const bool dark = FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
                         for (int row = 0; row < rows->size(); ++row) {
                             (*rows)[row].warningInitialized = false;
                             applyRow(row, dark);
                         }
                     });
    QObject::connect(table, &QTableWidget::itemSelectionChanged, window, selection);
    QObject::connect(edit, &QPushButton::clicked, window, [table, data, rows, window]() {
        const int row = table->currentRow();
        if (row >= 0 && row < rows->size()) {
            editParameterValue(data, rows->at(row).name, window);
        }
    });
    QObject::connect(closeBtn, &QPushButton::clicked, window, &QWidget::close);

    // Populate initial state once, then size columns once instead of live ResizeToContents on every update.
    const bool dark = FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
    for (int row = 0; row < rows->size(); ++row) {
        applyRow(row, dark);
    }
    auto *header = table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    table->resizeColumnsToContents();
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    table->setColumnWidth(1, std::max(table->columnWidth(1), 110));
    table->setColumnWidth(3, std::max(table->columnWidth(3), 90));
    table->setColumnWidth(5, std::max(table->columnWidth(5), 110));
    selection();

    QSize size(1050, 580);
    if (QScreen *screen = host && host->screen() ? host->screen() : QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        size = size.boundedTo(avail.size() * 0.9);
        window->resize(size);
        const QPoint center = host ? host->frameGeometry().center() : avail.center();
        const QPoint topLeft(std::clamp(center.x() - size.width() / 2, avail.left(), avail.right() - size.width()),
                             std::clamp(center.y() - size.height() / 2, avail.top(), avail.bottom() - size.height()));
        window->move(topLeft);
    } else {
        window->resize(size);
    }
    window->show();
    window->raise();
    window->activateWindow();
}
} // namespace fhmi
