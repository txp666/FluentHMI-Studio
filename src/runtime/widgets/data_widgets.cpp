#include "runtime/widgets/data_widgets.h"

#include "core/chart_file.h"
#include "core/hex_utils.h"
#include "core/protocol_parser.h"
#include "runtime/runtime_context.h"
#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/StyleSheet.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/RealtimePlotWidget.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SmoothScrollDelegate.h>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>

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

namespace {

constexpr int kHistoryLimit = 10000; // samples kept in memory, and the most 显示点数 shows
constexpr int kRefreshMs = 33;
// The table starts wide enough for these, so typical readings never resize the plot.
const char *const kSampleReading = "-0000.00";
const char *const kSampleMode = "游标: x=00000";

// PyQt UI Designer's _CURVE_COLORS and _DARK_CURVE_COLORS.
const QColor kCurveColors[] = {{0, 120, 212},  {244, 67, 54}, {76, 175, 80}, {255, 152, 0},
                               {156, 39, 176}, {0, 188, 212}, {121, 85, 72}, {96, 125, 139}};
const QColor kDarkCurveColors[] = {{74, 163, 223},  {255, 112, 105}, {129, 199, 132}, {255, 183, 77},
                                   {206, 147, 216}, {77, 208, 225},  {188, 170, 164}, {176, 190, 197}};

// A reading as Python's f"{value:.4g}" writes it (std::to_chars rounds exactly as it does).
QString reading(double value)
{
    char text[32];
    const auto end = std::to_chars(text, text + sizeof text, value, std::chars_format::general, 4).ptr;
    return QString::fromLatin1(text, end - text);
}

QString valueRow(const QString &name, const QColor &color, const QString &value)
{
    return QStringLiteral("<b style='color:rgb(%1, %2, %3)'>%4</b>  %5")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(name.toHtmlEscaped(), value);
}

} // namespace

// The chart's plot: RealtimePlotWidget with what the Python chart's plot has besides, drawn over it: a
// loaded run as dashed curves, and a vertical cursor that a click places and a drag moves. While the user
// has not panned or zoomed, the view spans every curve, the dashed ones included.
class ChartPlot : public FluentQt::RealtimePlotWidget
{
  public:
    explicit ChartPlot(QWidget *parent) : FluentQt::RealtimePlotWidget(parent)
    {
        setMinimumSize(1, 1); // the chart may be smaller than the plot's own minimum
        connect(this, &RealtimePlotWidget::rangeChanged, this, [this]() {
            if (m_fitting) {
                return;
            }
            if (autoScroll() || autoYRange()) { // the plot's 自动范围, or a double click
                m_autoView = true;
                fitView();
                return;
            }
            const qreal view[4] = {xMinimum(), xMaximum(), yMinimum(), yMaximum()};
            if (!std::equal(std::begin(view), std::end(view), std::begin(m_fitted))) {
                m_autoView = false; // panned or zoomed
            } else if (m_autoView) {
                fitView(); // a curve hidden or shown in the legend
            }
        });
        fitView();
    }

    // The cursor was placed or dragged to x, or (with a double click) taken away.
    std::function<void(double x)> cursorMoved;
    std::function<void()> cursorCleared;

    // Each series' window of samples, and the dashed curves', all starting at x = 0.
    void showWindows(const QList<QList<double>> &live, const QList<QList<double>> &references,
                     const QList<QColor> &referenceColors)
    {
        m_live = live;
        m_references = references;
        m_referenceColors = referenceColors;
        for (int i = 0; i < seriesCount() && i < live.size(); ++i) {
            setSamples(i, live.at(i));
        }
        fitView();
    }

    void hideCursor()
    {
        m_cursorOn = false;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        RealtimePlotWidget::paintEvent(event);
        const QRectF plot = plotArea();
        if (plot.width() <= 2 || plot.height() <= 2) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(plot.adjusted(1, 1, -1, -1));
        for (int i = 0; i < m_references.size(); ++i) {
            painter.setPen(QPen(m_referenceColors.value(i), 2, Qt::DashLine));
            painter.drawPolyline(polyline(m_references.at(i), plot));
        }
        if (m_cursorOn) {
            const qreal x = toPixelX(m_cursorX, plot);
            painter.setPen(QPen(QColor(255, 0, 0, 180), 1, Qt::DashLine));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        if (event->button() == Qt::LeftButton && onCursor(position)) {
            m_draggingCursor = true;
            event->accept();
            return;
        }
        const QList<bool> shown = visibility();
        RealtimePlotWidget::mousePressEvent(event);
        // A click on a legend entry hides or shows a curve; anywhere else in the plot it may place the cursor.
        m_clickPending = event->button() == Qt::LeftButton && plotArea().contains(position) && visibility() == shown;
        m_pressPosition = position;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF position = event->position();
        if (m_draggingCursor) {
            placeCursor(position.x());
            event->accept();
            return;
        }
        if (m_clickPending) {
            // Not a drag yet: a click must not pan the plot by the pixel a hand shakes.
            if ((position - m_pressPosition).manhattanLength() < QApplication::startDragDistance()) {
                event->accept();
                return;
            }
            m_clickPending = false;
        }
        if (event->buttons() == Qt::NoButton) {
            if (onCursor(position)) {
                setCursor(Qt::SizeHorCursor);
            } else {
                unsetCursor();
            }
        }
        RealtimePlotWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_draggingCursor && event->button() == Qt::LeftButton) {
            m_draggingCursor = false;
            event->accept();
            return;
        }
        const bool click = m_clickPending && event->button() == Qt::LeftButton;
        m_clickPending = false;
        RealtimePlotWidget::mouseReleaseEvent(event);
        if (click) {
            placeCursor(event->position().x());
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && plotArea().contains(event->position())) {
            m_clickPending = false;
            hideCursor();
            if (cursorCleared) {
                cursorCleared();
            }
        }
        RealtimePlotWidget::mouseDoubleClickEvent(event); // and the view follows the data again
    }

  private:
    // RealtimePlotWidget's plotRect(), which is private.
    QRectF plotArea() const { return QRectF(rect()).adjusted(58, 18, -58, -42); }

    // The view is always an explicit range here (fitView sets one), so the getters are what is shown.
    qreal toPixelX(qreal x, const QRectF &plot) const
    {
        return plot.left() + (x - xMinimum()) / std::max(1e-6, xMaximum() - xMinimum()) * plot.width();
    }

    qreal toPixelY(qreal y, const QRectF &plot) const
    {
        return plot.bottom() - (y - yMinimum()) / std::max(1e-6, yMaximum() - yMinimum()) * plot.height();
    }

    bool onCursor(const QPointF &position) const
    {
        const QRectF plot = plotArea();
        return m_cursorOn && plot.contains(position) && std::abs(position.x() - toPixelX(m_cursorX, plot)) <= 4;
    }

    void placeCursor(qreal pixelX)
    {
        const QRectF plot = plotArea();
        const qreal x = std::clamp(pixelX, plot.left(), plot.right());
        m_cursorOn = true;
        m_cursorX = xMinimum() + (x - plot.left()) / std::max<qreal>(1, plot.width()) * (xMaximum() - xMinimum());
        update();
        if (cursorMoved) {
            cursorMoved(m_cursorX);
        }
    }

    QList<bool> visibility() const
    {
        QList<bool> shown;
        for (int i = 0; i < seriesCount(); ++i) {
            shown.append(isSeriesVisible(i));
        }
        return shown;
    }

    // The visible stretch of a dashed curve; where a pixel column holds many samples, their lowest and
    // highest are enough.
    QPolygonF polyline(const QList<double> &values, const QRectF &plot) const
    {
        QPolygonF line;
        if (values.isEmpty()) {
            return line;
        }
        const qsizetype last = values.size() - 1;
        const qsizetype from = std::clamp<qsizetype>(qsizetype(std::floor(xMinimum())) - 1, 0, last);
        const qsizetype to = std::clamp<qsizetype>(qsizetype(std::ceil(xMaximum())) + 1, 0, last);
        if (to - from + 1 <= 2 * plot.width()) {
            for (qsizetype i = from; i <= to; ++i) {
                line << QPointF(toPixelX(i, plot), toPixelY(values.at(i), plot));
            }
            return line;
        }
        int column = -1;
        double low = 0, high = 0;
        const auto flush = [&]() {
            if (column >= 0) {
                line << QPointF(column, toPixelY(low, plot)) << QPointF(column, toPixelY(high, plot));
            }
        };
        for (qsizetype i = from; i <= to; ++i) {
            const int at = int(toPixelX(i, plot));
            const double value = values.at(i);
            if (at != column) {
                flush();
                column = at;
                low = high = value;
            } else {
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
        flush();
        return line;
    }

    // Spans every sample of the shown curves, dashed ones included, with the plot's own vertical margin.
    void fitView()
    {
        if (!m_autoView) {
            update();
            return;
        }
        qsizetype count = 0;
        double low = std::numeric_limits<double>::max();
        double high = std::numeric_limits<double>::lowest();
        const auto take = [&](const QList<double> &values) {
            count = std::max(count, values.size());
            for (const double value : values) {
                low = std::min(low, value);
                high = std::max(high, value);
            }
        };
        for (int i = 0; i < m_live.size(); ++i) {
            if (isSeriesVisible(i)) {
                take(m_live.at(i));
            }
        }
        for (const QList<double> &values : std::as_const(m_references)) {
            take(values);
        }
        qreal yLow = -1, yHigh = 1; // the plot's range while it has no data
        if (low <= high) {
            const qreal padding = std::max(0.2, std::max(1e-6, high - low) * 0.12);
            yLow = low - padding;
            yHigh = high + padding;
        }
        m_fitting = true;
        setXRange(0, qreal(std::max<qsizetype>(1, count - 1)));
        setYRange(yLow, yHigh);
        m_fitting = false;
        m_fitted[0] = xMinimum();
        m_fitted[1] = xMaximum();
        m_fitted[2] = yMinimum();
        m_fitted[3] = yMaximum();
        update();
    }

    QList<QList<double>> m_live;
    QList<QList<double>> m_references;
    QList<QColor> m_referenceColors;
    bool m_cursorOn = false;
    qreal m_cursorX = 0;
    bool m_draggingCursor = false;
    bool m_clickPending = false;
    QPointF m_pressPosition;
    bool m_autoView = true;
    bool m_fitting = false;
    qreal m_fitted[4] = {};
};

RealtimeChartWidget::RealtimeChartWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(2);

    // The plot with its title (left), the value table (right).
    auto *main = new QHBoxLayout;
    main->setSpacing(4);
    auto *plotColumn = new QVBoxLayout;
    plotColumn->setSpacing(0);
    m_title = new QLabel(this);
    m_title->setAlignment(Qt::AlignCenter);
    m_plot = new ChartPlot(this);
    m_plot->setLegendVisible(true);
    plotColumn->addWidget(m_title);
    plotColumn->addWidget(m_plot, 1);
    main->addLayout(plotColumn, 1);
    m_table = new QWidget(this);
    auto *table = new QVBoxLayout(m_table);
    table->setContentsMargins(2, 0, 2, 0);
    table->setSpacing(0);
    m_mode = new QLabel(tr("实时"), m_table);
    m_mode->setAlignment(Qt::AlignCenter);
    table->addWidget(m_mode);
    m_rows = new QVBoxLayout;
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(0);
    table->addLayout(m_rows);
    table->addStretch();
    main->addWidget(m_table);
    root->addLayout(main, 1);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(4, 2, 4, 0);
    toolbar->setSpacing(6);
    toolbar->addWidget(new FluentQt::BodyLabel(tr("显示点数"), this));
    m_displayEdit = new FluentQt::LineEdit(this);
    m_displayEdit->setFixedWidth(80);
    toolbar->addWidget(m_displayEdit);
    toolbar->addStretch();
    auto *save = new FluentQt::PushButton(this);
    save->setText(tr("保存"));
    save->setFixedWidth(64);
    auto *load = new FluentQt::PushButton(this);
    load->setText(tr("加载"));
    load->setFixedWidth(64);
    m_clearReference = new FluentQt::PushButton(this);
    m_clearReference->setText(tr("清除对比"));
    m_clearReference->hide();
    toolbar->addWidget(save);
    toolbar->addWidget(load);
    toolbar->addWidget(m_clearReference);
    root->addLayout(toolbar);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshMs);
    m_refreshTimer->setSingleShot(true);
    connect(m_refreshTimer, &QTimer::timeout, this, &RealtimeChartWidget::refresh);

    connect(m_displayEdit, &QLineEdit::editingFinished, this, [this]() {
        bool ok = false;
        const int typed = m_displayEdit->text().trimmed().toInt(&ok);
        m_displayPoints = std::clamp(ok ? typed : m_displayPoints, 10, kHistoryLimit);
        m_displayEdit->setText(QString::number(m_displayPoints));
        m_plot->setCapacity(m_displayPoints);
        refresh();
    });
    connect(save, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, tr("保存完整曲线数据"), QString(),
                                                          tr("CSV文件 (*.csv);;JSON文件 (*.json)"));
        if (!path.isEmpty()) {
            saveHistory(path);
        }
    });
    connect(load, &QPushButton::clicked, this, [this]() {
        const QString path =
            QFileDialog::getOpenFileName(this, tr("加载对比曲线"), QString(), tr("CSV文件 (*.csv);;JSON文件 (*.json)"));
        if (!path.isEmpty()) {
            loadReference(path);
        }
    });
    connect(m_clearReference, &QPushButton::clicked, this, [this]() {
        clearReference();
        refresh();
    });
    m_plot->cursorMoved = [this](double x) {
        m_cursorMode = true;
        m_cursorX = x;
        showCursorValues();
    };
    m_plot->cursorCleared = [this]() {
        m_cursorMode = false;
        m_mode->setText(tr("实时"));
        showLiveValues();
    };
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this,
            [this]() { applyTheme(); });
}

RealtimeChartWidget::~RealtimeChartWidget() = default;

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
            {QStringLiteral("mbArea"), field("mbArea", ModbusArea::HoldingRegisters)},
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
    m_title->setVisible(!text("title").isEmpty());
    m_displayPoints = std::clamp(integer("displayPoints"), 10, kHistoryLimit);
    m_displayEdit->setText(QString::number(m_displayPoints));
    m_plot->setCapacity(m_displayPoints);
    m_curves = curvesFromProperties(properties());
    rebuildCurves();
}

void RealtimeChartWidget::rebuildCurves()
{
    clearReference(); // its rows are part of the table rebuilt here
    // A curve keeps its samples while the curves are reconfigured; only removed curves lose theirs.
    while (m_histories.size() > m_curves.size()) {
        m_histories.pop_back();
    }
    while (m_histories.size() < m_curves.size()) {
        m_histories.push_back(std::make_unique<ChartHistory>(kHistoryLimit));
    }
    m_plot->clearSeries();
    qDeleteAll(m_valueLabels);
    m_valueLabels.clear();
    for (size_t i = 0; i < m_curves.size(); ++i) {
        m_plot->addSeries(m_curves[i].name, curveColor(int(i)));
        // Sized for a typical reading up front; applyTheme then shows the real values.
        auto *label =
            new QLabel(valueRow(m_curves[i].name, curveColor(int(i)), QLatin1String(kSampleReading)), m_table);
        m_rows->addWidget(label);
        m_valueLabels.append(label);
    }
    m_cursorMode = false;
    m_plot->hideCursor();
    m_mode->setText(tr("实时"));
    m_table->setMinimumWidth(0);
    holdTableWidth();
    applyTheme();
}

void RealtimeChartWidget::applyTheme()
{
    const bool dark = isDarkTheme();
    const QString foreground = dark ? QStringLiteral("#e5e5e5") : QStringLiteral("#202020");
    const QString accent = dark ? QStringLiteral("#4aa3df") : QStringLiteral("#0078d4");
    m_title->setStyleSheet(QStringLiteral("font-size: 10pt; color: %1;").arg(foreground));
    m_mode->setStyleSheet(QStringLiteral("font-size: 10px; color: %1; padding: 1px 0;").arg(accent));
    for (QLabel *label : m_valueLabels + m_referenceLabels) {
        label->setStyleSheet(QStringLiteral("font-size: 11px; padding: 1px 2px; color: %1;").arg(foreground));
    }
    for (int i = 0; i < m_plot->seriesCount(); ++i) {
        m_plot->setSeriesColor(i, curveColor(i));
    }
    refresh();
}

QColor RealtimeChartWidget::curveColor(int index) const
{
    const QColor *colors = isDarkTheme() ? kDarkCurveColors : kCurveColors;
    return colors[index % int(std::size(kCurveColors))];
}

void RealtimeChartWidget::appendSample(int curve, double value)
{
    if (curve < 0 || curve >= int(m_histories.size())) {
        return;
    }
    QString error;
    if (!m_histories[size_t(curve)]->append(value, &error)) {
        if (!error.isEmpty()) {
            m_mode->setText(tr("记录失败: %1").arg(error));
        }
        return;
    }
    if (!m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
}

qint64 RealtimeChartWidget::windowStart() const
{
    qint64 count = 0;
    for (const auto &history : m_histories) {
        count = std::max(count, history->count());
    }
    return std::max<qint64>(0, count - m_displayPoints);
}

void RealtimeChartWidget::refresh()
{
    m_refreshTimer->stop();
    QList<QList<double>> live;
    for (const auto &history : m_histories) {
        live.append(history->tail(m_displayPoints));
    }
    // A loaded run lines up sample for sample with the live window.
    const qint64 start = windowStart();
    QList<QList<double>> references;
    QList<QColor> colors;
    for (int i = 0; i < m_references.size(); ++i) {
        references.append(m_references.at(i).values.mid(start, m_displayPoints));
        colors.append(curveColor(int(m_curves.size()) + i));
    }
    m_plot->showWindows(live, references, colors);
    if (m_cursorMode) {
        showCursorValues();
    } else {
        showLiveValues();
    }
}

void RealtimeChartWidget::showLiveValues()
{
    qint64 count = 0;
    for (int i = 0; i < m_valueLabels.size() && size_t(i) < m_curves.size(); ++i) {
        const std::optional<double> last = m_histories[size_t(i)]->last();
        m_valueLabels[i]->setText(
            valueRow(m_curves[size_t(i)].name, curveColor(i), last ? reading(*last) : QStringLiteral("--")));
        count = std::max(count, m_histories[size_t(i)]->count());
    }
    showReferenceValues(count - 1); // the loaded run where the live one has got to
    holdTableWidth();
}

void RealtimeChartWidget::showCursorValues()
{
    // Python's round(): halves go to the even neighbour.
    const qint64 index = qint64(std::nearbyint(m_cursorX));
    m_mode->setText(tr("游标: x=%1").arg(index));
    for (int i = 0; i < m_valueLabels.size() && size_t(i) < m_curves.size(); ++i) {
        const QList<double> shown = m_histories[size_t(i)]->tail(m_displayPoints);
        const QString value = index >= 0 && index < shown.size() ? reading(shown.at(index)) : QStringLiteral("--");
        m_valueLabels[i]->setText(valueRow(m_curves[size_t(i)].name, curveColor(i), value));
    }
    showReferenceValues(windowStart() + index);
    holdTableWidth();
}

void RealtimeChartWidget::showReferenceValues(qint64 sample)
{
    for (int i = 0; i < m_referenceLabels.size(); ++i) {
        const QList<double> &values = m_references.at(i).values;
        const QString value = sample >= 0 && sample < values.size() ? reading(values.at(sample)) : QStringLiteral("--");
        m_referenceLabels[i]->setText(valueRow(m_references.at(i).name, curveColor(int(m_curves.size()) + i), value));
    }
}

void RealtimeChartWidget::holdTableWidth()
{
    int need = 0;
    m_mode->ensurePolished();
    need = m_mode->fontMetrics().horizontalAdvance(QString::fromUtf8(kSampleMode));
    for (QLabel *label : m_valueLabels + m_referenceLabels) {
        label->ensurePolished();
        need = std::max(need, label->sizeHint().width());
    }
    const QMargins margins = m_table->layout()->contentsMargins();
    need += margins.left() + margins.right();
    if (need > m_table->minimumWidth()) {
        m_table->setMinimumWidth(need);
    }
}

void RealtimeChartWidget::saveHistory(const QString &path)
{
    QStringList names;
    for (const Curve &curve : m_curves) {
        names.append(curve.name);
    }
    bool unreadable = false;
    QList<ChartSamples> samples;
    for (const auto &history : m_histories) {
        samples.append(history->samples(&unreadable));
    }
    QSaveFile file(path);
    QString error;
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
    } else if (!writeChartFile(&file, chartFileFormat(path), names, samples, &error)) {
        file.cancelWriting();
    } else if (unreadable) {
        error = tr("无法读取曲线历史");
        file.cancelWriting();
    } else if (!file.commit()) {
        error = file.errorString();
    }
    if (!error.isEmpty()) {
        m_mode->setText(tr("保存失败: %1").arg(error));
    }
}

void RealtimeChartWidget::loadReference(const QString &path)
{
    QFile file(path);
    QStringList names;
    QList<QList<double>> values;
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
    } else if (!readChartFile(file.readAll(), chartFileFormat(path), &names, &values, &error) && error.isEmpty()) {
        error = tr("无法读取");
    }
    if (!error.isEmpty()) {
        m_mode->setText(tr("加载失败: %1").arg(error));
        return;
    }
    clearReference();
    const QString stem = QFileInfo(path).completeBaseName();
    for (int i = 0; i < names.size(); ++i) {
        m_references.append({QStringLiteral("%1（%2）").arg(names.at(i), stem), values.value(i)});
        auto *label = new QLabel(m_table);
        m_rows->addWidget(label);
        m_referenceLabels.append(label);
    }
    m_clearReference->show();
    applyTheme();
}

void RealtimeChartWidget::clearReference()
{
    qDeleteAll(m_referenceLabels);
    m_referenceLabels.clear();
    m_references.clear();
    m_clearReference->hide();
}

void RealtimeChartWidget::onAttached()
{
    // Modbus curves are read by automatic polling (ModbusPoller), at its default period.
    QList<ModbusNeed> needs;
    for (const Curve &curve : m_curves) {
        if (const auto need = modbusNeed(curve.binding)) {
            needs.append(*need);
        }
    }
    context()->setCurveNeeds(this, needs);
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
                appendSample(int(i), *y);
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
                    appendSample(int(i), *y);
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
    context()->setCurveNeeds(this, {});
    for (const auto &history : m_histories) {
        history->clear();
    }
    refresh();
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
