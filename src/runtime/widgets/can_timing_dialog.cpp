#include "runtime/widgets/can_timing_dialog.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>

#include <QtCore/QRegularExpression>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

#include <cmath>

namespace fhmi {
namespace {

QString btrText(quint16 btr)
{
    return QStringLiteral("%1,%2")
        .arg(btr >> 8, 2, 16, QLatin1Char('0'))
        .arg(btr & 255, 2, 16, QLatin1Char('0'))
        .toUpper();
}

} // namespace

CanTimingDialog::CanTimingDialog(const CanSettings &settings, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("canTimingCalculator"));
    setWindowTitle(tr("CAN 位时序计算器"));
    resize(800, 560);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    auto *caption = new FluentQt::BodyLabel(tr("USBCAN-I/II · 晶振 16 MHz（位时序时钟 8 MHz）"), this);
    layout->addWidget(caption);
    auto *inputs = new QGridLayout;
    m_rate = new FluentQt::LineEdit(this);
    m_rate->setObjectName(QStringLiteral("timingCalculatorRate"));
    m_rate->setMaxLength(32);
    m_rate->setPlaceholderText(QStringLiteral("500k / 125000 / 1M"));
    m_sample = new FluentQt::DoubleSpinBox(this);
    m_sample->setObjectName(QStringLiteral("timingCalculatorSample"));
    m_sample->setRange(1, 99);
    m_sample->setDecimals(2);
    m_sample->setSuffix(QStringLiteral("%"));
    m_sample->setValue(settings.samplePoint * 100);
    m_sjw = new FluentQt::SpinBox(this);
    m_sjw->setObjectName(QStringLiteral("timingCalculatorSjw"));
    m_sjw->setRange(1, 4);
    m_sjw->setValue(1);
    m_error = new FluentQt::DoubleSpinBox(this);
    m_error->setObjectName(QStringLiteral("timingCalculatorError"));
    m_error->setRange(0, 1);
    m_error->setDecimals(3);
    m_error->setSingleStep(0.05);
    m_error->setSuffix(QStringLiteral("%"));
    m_error->setValue(0.1);
    m_triple = new FluentQt::CheckBox(tr("三次采样"), this);
    m_triple->setObjectName(QStringLiteral("timingCalculatorTriple"));
    m_rate->setText(CanAdapter::bitrateText(settings.bitrate));
    const QByteArray name = settings.zlgModel.toLatin1();
    if (const auto *model = zlg::findModel(name.constData(), name.size())) {
        const auto timing = zlg::channelTiming(*model, settings);
        if (timing && timing->btr && !settings.customTiming.isEmpty()) {
            const quint16 btr = *timing->btr;
            const int brp = ((btr >> 8) & 63) + 1;
            const int tseg1 = (btr & 15) + 1;
            const int tseg2 = ((btr >> 4) & 7) + 1;
            m_rate->setText(QString::number(8000000.0 / (brp * (1 + tseg1 + tseg2)), 'f', 3));
            m_sample->setValue(double(1 + tseg1) * 100 / (1 + tseg1 + tseg2));
            m_sjw->setValue((btr >> 14) + 1);
            m_triple->setChecked(btr & 0x80);
        }
    }
    const auto input = [&](int row, int column, const QString &label, QWidget *field) {
        inputs->addWidget(new FluentQt::BodyLabel(label, this), row, column);
        inputs->addWidget(field, row, column + 1);
    };
    input(0, 0, tr("目标波特率"), m_rate);
    input(0, 2, tr("目标采样点"), m_sample);
    input(1, 0, QStringLiteral("SJW"), m_sjw);
    input(1, 2, tr("最大速率误差"), m_error);
    inputs->addWidget(m_triple, 1, 4);
    layout->addLayout(inputs);

    m_table = new FluentQt::TableWidget(this);
    m_table->setObjectName(QStringLiteral("timingCalculatorResults"));
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels({tr("实际速率(kbps)"), tr("采样点"), tr("速率误差"), QStringLiteral("BRP"),
                                        QStringLiteral("TSEG1"), QStringLiteral("TSEG2"), QStringLiteral("SJW"),
                                        QStringLiteral("BTR0,BTR1")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);
    m_status = new FluentQt::CaptionLabel(this);
    m_status->setObjectName(QStringLiteral("timingCalculatorStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    m_copy = new FluentQt::PushButton(tr("复制参数"), this);
    m_copy->setObjectName(QStringLiteral("timingCalculatorCopy"));
    auto *cancel = new FluentQt::PushButton(tr("取消"), this);
    cancel->setObjectName(QStringLiteral("timingCalculatorCancel"));
    m_apply = new FluentQt::PrimaryPushButton(tr("应用到卡片"), this);
    m_apply->setObjectName(QStringLiteral("timingCalculatorApply"));
    m_apply->setDefault(true);
    buttons->addWidget(m_copy);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(m_apply);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, [this]() {
        if (!selectedTiming().isEmpty()) {
            accept();
        }
    });
    connect(m_copy, &QPushButton::clicked, this, [this]() { QGuiApplication::clipboard()->setText(selectedTiming()); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &CanTimingDialog::updateSelection);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this]() { m_apply->click(); });
    connect(m_rate, &QLineEdit::textChanged, this, &CanTimingDialog::calculate);
    connect(m_sample, &QDoubleSpinBox::valueChanged, this, &CanTimingDialog::calculate);
    connect(m_sjw, &QSpinBox::valueChanged, this, &CanTimingDialog::calculate);
    connect(m_error, &QDoubleSpinBox::valueChanged, this, &CanTimingDialog::calculate);
    connect(m_triple, &QCheckBox::toggled, this, &CanTimingDialog::calculate);
    calculate();
}

void CanTimingDialog::calculate()
{
    static const QRegularExpression syntax(QStringLiteral("^([0-9]+(?:\\.[0-9]+)?)\\s*([kKmM]?)$"));
    const auto match = syntax.match(m_rate->text().trimmed());
    const double multiplier = match.captured(2).compare(QStringLiteral("k"), Qt::CaseInsensitive) == 0   ? 1000
                              : match.captured(2).compare(QStringLiteral("M"), Qt::CaseInsensitive) == 0 ? 1000000
                                                                                                         : 1;
    const double value = match.captured(1).toDouble() * multiplier;
    const bool valid = match.hasMatch() && std::isfinite(value) && value >= 1 && value <= 1000000;
    m_candidates = valid ? zlg::sjaTimingCandidates(value, m_sample->value() / 100, m_sjw->value(),
                                                    int(std::llround(m_error->value() * 10000)))
                         : QList<zlg::SjaTimingCandidate>{};
    m_table->setRowCount(m_candidates.size());
    for (int row = 0; row < m_candidates.size(); ++row) {
        const auto &candidate = m_candidates.at(row);
        const auto &timing = candidate.timing;
        const QStringList fields{QString::number(candidate.bitrate / 1000, 'f', 6),
                                 QString::number(timing.samplePoint() * 100, 'f', 2) + QLatin1Char('%'),
                                 QString::number(candidate.bitrateErrorPpm / 10000, 'f', 6) + QLatin1Char('%'),
                                 QString::number(timing.brp),
                                 QString::number(timing.tseg1),
                                 QString::number(timing.tseg2),
                                 QString::number(timing.sjw),
                                 btrText(candidate.btr | (m_triple->isChecked() ? 0x80 : 0))};
        for (int column = 0; column < fields.size(); ++column) {
            m_table->setItem(row, column, new QTableWidgetItem(fields.at(column)));
        }
    }
    if (!m_candidates.isEmpty()) {
        m_table->selectRow(0);
    }
    m_status->setText(
        !valid ? tr("请输入 1–1000000 bps 的波特率，例如 500k 或 1M")
        : m_candidates.isEmpty()
            ? tr("没有满足误差要求的时序，请调整速率或最大速率误差")
            : tr("共 %1 个结果，按速率和采样点误差排序；应用后点击连接才会生效").arg(m_candidates.size()));
    updateSelection();
}

QString CanTimingDialog::selectedTiming() const
{
    const int row = m_table->currentRow();
    return row >= 0 && row < m_candidates.size()
               ? btrText(m_candidates.at(row).btr | (m_triple->isChecked() ? 0x80 : 0))
               : QString();
}

void CanTimingDialog::updateSelection()
{
    const bool selected = !selectedTiming().isEmpty();
    m_copy->setEnabled(selected);
    m_apply->setEnabled(selected);
}

} // namespace fhmi
