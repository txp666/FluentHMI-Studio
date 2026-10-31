#include "runtime/widgets/data_widgets.h"

#include "core/hex_utils.h"
#include "core/protocol_parser.h"
#include "runtime/runtime_context.h"
#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/RealtimePlotWidget.h>
#include <FluentQtWidgets/StyleSheet.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SmoothScrollDelegate.h>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

const QHash<QString, QString> &logColors()
{
    static const QHash<QString, QString> colors = {{QStringLiteral("info"), QStringLiteral("#a8d8ea")},
                                                   {QStringLiteral("warn"), QStringLiteral("#FFD54F")},
                                                   {QStringLiteral("error"), QStringLiteral("#EF5350")},
                                                   {QStringLiteral("ok"), QStringLiteral("#81C784")},
                                                   {QStringLiteral("sent"), QStringLiteral("#F9E2AF")},
                                                   {QStringLiteral("system"), QStringLiteral("#9399b2")}};
    return colors;
}

void paintCardBackground(QWidget *widget)
{
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

QString messageText(const Message &message)
{
    QStringList parts;
    for (auto it = message.constBegin(); it != message.constEnd(); ++it) {
        parts.append(QStringLiteral("%1=%2").arg(it.key(), displayString(it.value())));
    }
    return parts.join(QStringLiteral(", "));
}

} // namespace

QTextEdit *createTerminal(QWidget *parent)
{
    auto *edit = new FluentQt::TextEdit(parent);
    edit->setReadOnly(true);
    // Received messages keep their colours, so the terminal stays dark in both themes. The
    // selectors match Fluent's own TextEdit rules, which would otherwise win on specificity.
    const QString qss = QStringLiteral(
        "QTextEdit[fqw=\"TextEdit\"], QTextEdit[fqw=\"TextEdit\"]:hover, QTextEdit[fqw=\"TextEdit\"]:focus {"
        " font: 12px Menlo, Consolas, 'Courier New', monospace; color: #cdd6f4; background-color: #1e1e2e;"
        " border: 1px solid #333; border-radius: 6px; padding: 4px; }");
    FluentQt::FluentStyleSheet::setCustomStyleSheet(edit, qss, qss);
    if (QWidget *layer = edit->layer()) {
        layer->hide(); // the focus underline belongs to input fields, not to a read-only log
    }
    // Fluent's light-theme scroll bar (a dark handle on a white groove) vanishes on the
    // dark text and glares when hovered; give it terminal colours in both themes.
    if (FluentQt::SmoothScrollDelegate *delegate = edit->scrollDelegate()) {
        const QColor handle(168, 216, 234, 110), arrow(168, 216, 234, 160), groove(36, 36, 60, 235);
        for (FluentQt::ScrollBar *bar : {static_cast<FluentQt::ScrollBar *>(delegate->vScrollBar()),
                                         static_cast<FluentQt::ScrollBar *>(delegate->hScrollBar())}) {
            if (bar) {
                bar->setHandleColor(handle, handle);
                bar->setArrowColor(arrow, arrow);
                bar->setGrooveColor(groove, groove);
            }
        }
    }
    return edit;
}

QString terminalEscape(const QString &text)
{
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
}

// ---------------------------------------------------------------------------
// RealtimeChart
// ---------------------------------------------------------------------------

RealtimeChartWidget::RealtimeChartWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *header = new QHBoxLayout;
    m_title = new FluentQt::StrongBodyLabel(this);
    auto *clear = new FluentQt::TransparentPushButton(this);
    clear->setText(tr("清空"));
    auto *save = new FluentQt::TransparentPushButton(this);
    save->setText(tr("导出 CSV"));
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(clear);
    header->addWidget(save);
    m_plot = new FluentQt::RealtimePlotWidget(this);
    m_plot->setLegendVisible(true);
    m_plot->setAutoYRange(true);
    m_plot->setCapacity(10000);
    layout->addLayout(header);
    layout->addWidget(m_plot, 1);
    connect(clear, &QPushButton::clicked, m_plot, &FluentQt::RealtimePlotWidget::clear);
    connect(save, &QPushButton::clicked, this, &RealtimeChartWidget::exportCsv);
}

std::vector<RealtimeChartWidget::Curve> RealtimeChartWidget::curvesFromProperties(const QJsonObject &props)
{
    const int count = std::clamp(props.value(QStringLiteral("curveCount")).toInt(1), 1, 8);
    std::vector<Curve> curves;
    for (int i = 0; i < count; ++i) {
        const auto field = [&props, i](const char *name, const QJsonValue &fallback) {
            const QJsonValue value = props.value(QStringLiteral("_curve_%1_%2").arg(i).arg(QLatin1String(name)));
            return value.isUndefined() || value.isNull() ? fallback : value;
        };
        const QJsonObject binding{
            {QStringLiteral("parseMode"), field("parseMode", ParseMode::KeyValue)},
            {QStringLiteral("key"), field("key", QString())},
            {QStringLiteral("jsonPath"), field("jsonPath", QString())},
            {QStringLiteral("byteOffset"), field("byteOffset", 0)},
            {QStringLiteral("dataType"), field("dataType", QStringLiteral("float32"))},
            {QStringLiteral("byteOrder"), field("byteOrder", QStringLiteral("大端(BE)"))},
            {QStringLiteral("frameHeader"), field("frameHeader", QString())},
            {QStringLiteral("frameTail"), field("frameTail", QString())},
            {QStringLiteral("mbRegIndex"), field("regIndex", 0)},
            {QStringLiteral("mbDataType"), field("mbDataType", QStringLiteral("uint16"))},
            {QStringLiteral("mbRegOrder"), field("mbRegOrder", QStringLiteral("大端(AB CD)"))},
            {QStringLiteral("mbFcFilter"), field("mbFcFilter", -1)},
            {QStringLiteral("mbSlaveFilter"), field("mbSlaveFilter", -1)},
            {QStringLiteral("mbArea"), field("mbArea", ModbusArea::InResponse)},
        };
        const QString name = field("name", QStringLiteral("曲线%1").arg(i + 1)).toString();
        auto entry = BindingEntry::fromWidget(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("RealtimeChart")}, {QStringLiteral("binding"), binding}});
        BindingEntry fallback;
        fallback.key = QString(); // matches nothing until configured
        curves.push_back({name, entry ? *entry : fallback});
    }
    return curves;
}

void RealtimeChartWidget::apply()
{
    m_title->setText(text("title"));
    m_plot->setMaximumVisiblePoints(std::max(10, integer("displayPoints")));
    m_curves = curvesFromProperties(properties());
    m_plot->clearSeries();
    static const QList<QColor> palette = {QColor(0x00, 0x9F, 0xAA), QColor(0xE8, 0x48, 0x3F), QColor(0x29, 0x8D, 0xF2),
                                          QColor(0xF7, 0xB9, 0x00), QColor(0x8E, 0x5C, 0xD9), QColor(0x3C, 0xC0, 0x6A),
                                          QColor(0xFF, 0x8C, 0x00), QColor(0x60, 0x7D, 0x8B)};
    for (size_t i = 0; i < m_curves.size(); ++i) {
        m_plot->addSeries(m_curves[i].name, palette.at(int(i) % palette.size()));
    }
}

void RealtimeChartWidget::onAttached()
{
    CommHub *hub = context()->hub();
    live(connect(hub, &CommHub::messageReceived, this, [this](const Message &message) {
        if (!context()->isActive()) {
            return;
        }
        for (size_t i = 0; i < m_curves.size(); ++i) {
            const BindingEntry &binding = m_curves[i].binding;
            if (!binding.usesParsedData()) {
                continue;
            }
            // A malformed sample skips this curve only; the frame's other curves still update.
            if (const auto y = bindingFloat(binding.evaluate(binding.extractFromParsed(message)))) {
                m_plot->appendSample(int(i), *y);
            }
        }
    }));
    live(connect(hub, &CommHub::rawReceived, this, [this](const QByteArray &bytes) {
        if (!context()->isActive()) {
            return;
        }
        for (size_t i = 0; i < m_curves.size(); ++i) {
            BindingEntry &binding = m_curves[i].binding;
            if (binding.usesParsedData()) {
                continue;
            }
            for (const QVariant &value : binding.extractAllFromRaw(bytes)) {
                if (const auto y = bindingFloat(value)) {
                    m_plot->appendSample(int(i), *y);
                }
            }
        }
    }));
    live(connect(hub, &CommHub::linkDisconnected, this, [this]() {
        for (Curve &curve : m_curves) {
            curve.binding.resetStream();
        }
    }));
}

void RealtimeChartWidget::onDetached()
{
    m_plot->clear();
}

void RealtimeChartWidget::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("导出曲线"), text("title") + QStringLiteral(".csv"),
                                                      tr("CSV 文件 (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (context()) {
            context()->notify(tr("导出失败"), file.errorString(), NotifyType::Error);
        }
        return;
    }
    QTextStream out(&file);
    QList<QVector<QPointF>> series;
    int rows = 0;
    out << "index";
    for (int i = 0; i < m_plot->seriesCount(); ++i) {
        out << ',' << m_plot->seriesName(i);
        series.append(m_plot->points(i));
        rows = std::max(rows, int(series.last().size()));
    }
    out << '\n';
    for (int row = 0; row < rows; ++row) {
        out << row;
        for (const QVector<QPointF> &points : series) {
            out << ',';
            if (row < points.size()) {
                out << QString::number(points.at(row).y(), 'g', 10);
            }
        }
        out << '\n';
    }
}

// ---------------------------------------------------------------------------
// DataTable
// ---------------------------------------------------------------------------

DataTableWidget::DataTableWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    m_table = new FluentQt::TableWidget(this);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_table);
}

bool DataTableWidget::logging() const
{
    return text("recordMode") == QStringLiteral("逐行记录");
}

void DataTableWidget::apply()
{
    m_columns.clear();
    for (const QString &line : text("columns").split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty()) {
            m_columns.append(line.trimmed());
        }
    }
    if (m_columns.isEmpty()) {
        m_columns = {QStringLiteral("字段"), QStringLiteral("值")};
    }
    m_table->setRowCount(0);
    m_table->setColumnCount(int(m_columns.size()));
    m_table->setHorizontalHeaderLabels(m_columns);
    m_table->verticalHeader()->setVisible(!logging());
    QHeaderView *header = m_table->horizontalHeader();
    for (int c = 0; c < m_columns.size(); ++c) {
        header->setSectionResizeMode(c, logging() && c < m_columns.size() - 1 ? QHeaderView::ResizeToContents
                                                                               : QHeaderView::Stretch);
    }
}

void DataTableWidget::onAttached()
{
    live(connect(context()->hub(), &CommHub::messageReceived, this, &DataTableWidget::onMessage));
}

void DataTableWidget::onMessage(const Message &message)
{
    if (!context() || !context()->isActive()) {
        return;
    }
    const int maxRows = std::max(10, integer("maxRows"));
    if (logging()) {
        // Each column takes the field it is named after, 时间 the arrival time; frames with
        // none of the named fields belong to another message and are skipped.
        const QString timeColumn = QStringLiteral("时间");
        bool relevant = false;
        for (const QString &column : std::as_const(m_columns)) {
            relevant = relevant || (column != timeColumn && message.contains(column));
        }
        if (!relevant) {
            return;
        }
        if (m_table->rowCount() >= maxRows) {
            m_table->removeRow(0);
        }
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
        for (int c = 0; c < m_columns.size(); ++c) {
            const QString &column = m_columns.at(c);
            const QString cell = column == timeColumn ? stamp
                                 : message.contains(column) ? displayString(message.value(column))
                                                            : QString();
            m_table->setItem(row, c, new QTableWidgetItem(cell));
        }
        m_table->scrollToBottom();
        return;
    }
    m_table->setRowCount(0);
    for (auto it = message.constBegin(); it != message.constEnd() && m_table->rowCount() < maxRows; ++it) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(it.key()));
        if (m_table->columnCount() > 1) {
            m_table->setItem(row, 1, new QTableWidgetItem(displayString(it.value())));
        }
    }
}

// ---------------------------------------------------------------------------
// LogBox
// ---------------------------------------------------------------------------

LogBoxWidget::LogBoxWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_output = createTerminal(this);
    layout->addWidget(m_output);
}

void LogBoxWidget::apply()
{
    m_output->document()->setMaximumBlockCount(std::max(50, integer("maxLines")));
}

void LogBoxWidget::appendLog(const QString &text, const QString &level)
{
    const QString color = logColors().value(level, logColors().value(QStringLiteral("info")));
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    m_output->append(QStringLiteral("<span style=\"color:#9399b2\">[%1]</span> <span style=\"color:%2\">%3</span>")
                         .arg(stamp, color, terminalEscape(text)));
}

void LogBoxWidget::onAttached()
{
    CommHub *hub = context()->hub();
    live(connect(hub, &CommHub::linkConnected, this,
                 [this](const QString &link) { appendLog(tr("已连接: %1").arg(link), QStringLiteral("ok")); }));
    live(connect(hub, &CommHub::linkDisconnected, this,
                 [this](const QString &link) { appendLog(tr("已断开: %1").arg(link), QStringLiteral("warn")); }));
    live(connect(hub, &CommHub::linkError, this,
                 [this](const QString &message) { appendLog(tr("错误: %1").arg(message), QStringLiteral("error")); }));
    live(connect(hub, &CommHub::linkInfo, this, [this](const QString &message) { appendLog(message); }));
    live(connect(hub, &CommHub::messageReceived, this, [this](const Message &message) {
        if (context()->isActive()) {
            appendLog(messageText(message));
        }
    }));
}

// ---------------------------------------------------------------------------
// DataRecvCard
// ---------------------------------------------------------------------------

DataRecvCardWidget::DataRecvCardWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);
    auto *titleRow = new QHBoxLayout;
    m_title = new FluentQt::StrongBodyLabel(this);
    m_counter = new FluentQt::CaptionLabel(this);
    titleRow->addWidget(m_title);
    titleRow->addStretch();
    titleRow->addWidget(m_counter);
    auto *options = new QHBoxLayout;
    m_hex = new FluentQt::CheckBox(tr("HEX显示"), this);
    m_autoScroll = new FluentQt::CheckBox(tr("自动滚动"), this);
    m_autoScroll->setChecked(true);
    auto *clear = new FluentQt::PushButton(this);
    clear->setText(tr("清空"));
    clear->setFixedWidth(64);
    options->addWidget(m_hex);
    options->addWidget(m_autoScroll);
    options->addStretch();
    options->addWidget(clear);
    m_output = createTerminal(this);
    layout->addLayout(titleRow);
    layout->addLayout(options);
    layout->addWidget(m_output, 1);
    connect(clear, &QPushButton::clicked, this, [this]() {
        m_output->clear();
        m_count = 0;
        m_counter->setText(tr("%1 条").arg(m_count));
    });
}

void DataRecvCardWidget::apply()
{
    m_title->setText(text("title"));
    m_counter->setText(tr("%1 条").arg(m_count));
    m_output->document()->setMaximumBlockCount(std::max(50, integer("maxLines")));
}

QString DataRecvCardWidget::render(const QByteArray &bytes, bool received) const
{
    if (m_hex->isChecked()) {
        return toHexString(bytes);
    }
    QString text = received ? context()->hub()->decodeRaw(bytes) : LineParser::decode(bytes, QStringLiteral("utf-8"));
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r'))) {
        text.chop(1);
    }
    return text;
}

void DataRecvCardWidget::append(const QString &html)
{
    QScrollBar *bar = m_output->verticalScrollBar();
    const int keep = bar->value();
    m_output->append(html);
    if (m_autoScroll->isChecked()) {
        bar->setValue(bar->maximum());
    } else {
        bar->setValue(keep);
    }
}

void DataRecvCardWidget::onAttached()
{
    CommHub *hub = context()->hub();
    // << received, >> sent, --- link events ---, coloured as in PyQt UI Designer.
    const auto traffic = [this](const QByteArray &bytes, bool received, const char *arrows, const char *color) {
        const QString body = render(bytes, received);
        if (body.isEmpty()) {
            return;
        }
        append(QStringLiteral("<span style=\"color:#9399b2\">[%1]</span> <span style=\"color:%2\">%3 %4</span>")
                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")), QLatin1String(color),
                        QLatin1String(arrows).toString().toHtmlEscaped(), terminalEscape(body)));
    };
    const auto event = [this](const QString &message) {
        append(QStringLiteral("<span style=\"color:#74c7ec\">[%1] --- %2 ---</span>")
                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), terminalEscape(message)));
    };
    live(connect(hub, &CommHub::rawReceived, this, [this, traffic](const QByteArray &bytes) {
        traffic(bytes, true, "<<", "#a6e3a1");
        ++m_count;
        m_counter->setText(tr("%1 条").arg(m_count));
    }));
    live(connect(hub, &CommHub::bytesSent, this,
                 [traffic](const QByteArray &bytes) { traffic(bytes, false, ">>", "#f9e2af"); }));
    live(connect(hub, &CommHub::linkConnected, this,
                 [event](const QString &link) { event(tr("已连接: %1").arg(link)); }));
    live(connect(hub, &CommHub::linkDisconnected, this, [event](const QString &) { event(tr("已断开")); }));
}

void DataRecvCardWidget::paintEvent(QPaintEvent *)
{
    paintCardBackground(this);
}

// ---------------------------------------------------------------------------
// DataSendCard
// ---------------------------------------------------------------------------

DataSendCardWidget::DataSendCardWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    m_title = new FluentQt::StrongBodyLabel(this);
    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);
    m_input = new FluentQt::LineEdit(this);
    m_input->setClearButtonEnabled(true);
    m_send = new FluentQt::PrimaryPushButton(this);
    m_send->setIcon(FluentQt::icon(FluentQt::FluentIcon::Send));
    m_send->setText(tr("发送"));
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_send);
    auto *options = new QHBoxLayout;
    options->setSpacing(8);
    m_hex = new FluentQt::CheckBox(tr("HEX模式"), this);
    m_newline = new FluentQt::ComboBox(this);
    m_newline->addItems({tr("无"), QStringLiteral("\\r\\n"), QStringLiteral("\\n"), QStringLiteral("\\r")});
    m_newline->setCurrentIndex(1);
    m_newline->setFixedWidth(80);
    options->addWidget(m_hex);
    options->addStretch();
    options->addWidget(new FluentQt::BodyLabel(tr("换行符"), this));
    options->addWidget(m_newline);
    layout->addWidget(m_title);
    layout->addLayout(inputRow);
    layout->addLayout(options);
    layout->addStretch();
    m_tipTimer = new QTimer(this);
    m_tipTimer->setSingleShot(true);
    connect(m_tipTimer, &QTimer::timeout, this, [this]() { m_send->setText(tr("发送")); });
    // HEX has no line ending to choose.
    connect(m_hex, &QCheckBox::toggled, m_newline, [this](bool hex) { m_newline->setEnabled(!hex); });
}

void DataSendCardWidget::apply()
{
    m_title->setText(text("title"));
    m_input->setPlaceholderText(text("placeholder"));
}

void DataSendCardWidget::onAttached()
{
    live(connect(m_send, &QPushButton::clicked, this, &DataSendCardWidget::sendInput));
    live(connect(m_input, &QLineEdit::returnPressed, this, &DataSendCardWidget::sendInput));
}

void DataSendCardWidget::flashTip(const QString &tip)
{
    m_send->setText(tip);
    m_tipTimer->start(1500);
}

void DataSendCardWidget::sendInput()
{
    const QString input = m_input->text().trimmed();
    if (input.isEmpty()) {
        flashTip(tr("请输入数据"));
        return;
    }
    if (!context()->hub()->isConnected()) {
        flashTip(tr("未连接"));
        return;
    }
    EncodedCommand command;
    if (m_hex->isChecked()) {
        const auto bytes = parseHex(input);
        if (!bytes || bytes->isEmpty()) {
            flashTip(tr("HEX格式错误"));
            return;
        }
        command.bytes = *bytes;
    } else {
        static const QStringList endings = {QString(), QStringLiteral("\r\n"), QStringLiteral("\n"),
                                            QStringLiteral("\r")};
        command.bytes = (input + endings.value(m_newline->currentIndex())).toUtf8();
    }
    if (context()->send(CommandResult::ok(command))) {
        m_input->selectAll();
    }
}

void DataSendCardWidget::paintEvent(QPaintEvent *)
{
    paintCardBackground(this);
}

} // namespace fhmi
