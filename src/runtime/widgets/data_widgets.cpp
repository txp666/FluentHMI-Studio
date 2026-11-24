#include "runtime/widgets/data_widgets.h"

#include "comm/can/can_link.h"
#include "core/can_frame.h"

#include "core/chart_file.h"
#include "core/hex_utils.h"
#include "core/manual_command.h"
#include "core/protocol_parser.h"
#include "runtime/runtime_context.h"
#include "runtime/receive_frame_preview.h"
#include "runtime/send_frame_fields.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/chart_viewport.h"
#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Layout/FlowLayout.h>
#include <FluentQtWidgets/StyleSheet.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SmoothScrollDelegate.h>

#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAbstractTextDocumentLayout>
#include <QtGui/QFontMetrics>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QTextBlock>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
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

QString withSource(const QString &text, const DataSource &source)
{
    QString name = source.connectionId;
    if (!source.endpoint.isEmpty())
        name += name.isEmpty() ? source.endpoint : QLatin1Char('/') + source.endpoint;
    return name.isEmpty() ? text : QStringLiteral("[%1] %2").arg(name, text);
}

bool acceptsEvent(const DataRoute &route, const DataSource &source)
{
    return source.endpoint.isEmpty() ? route.acceptsConnection(source.connectionId) : route.accepts(source);
}

const QString kTimeField = QStringLiteral("@time");

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
// A header entry starts wide enough for its name and this, so typical readings never move the others.
const char *const kSampleReading = "-0000.00";

// A reading as Python's f"{value:.4g}" writes it (std::to_chars rounds exactly as it does).
QString reading(double value)
{
    char text[32];
    const auto end = std::to_chars(text, text + sizeof text, value, std::chars_format::general, 4).ptr;
    return QString::fromLatin1(text, end - text);
}

} // namespace

// The chart's plot: RealtimePlotWidget with what the Python chart's plot has besides, drawn over it: a
// loaded run as dashed curves, and a vertical cursor that a click places and a drag moves. Its own legend and
// value tags are off, as the header above it reads the values. While the user has not panned or zoomed, the
// view spans every shown curve, the dashed ones included.
class ChartPlot : public ChartViewport
{
  public:
    explicit ChartPlot(QWidget *parent) : ChartViewport(parent)
    {
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
                fitView(); // a curve hidden or shown from the header
            }
        });
        fitView();
    }

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

    bool isReferenceVisible(int index) const { return !m_referenceHidden.value(index); }

    void setReferenceVisible(int index, bool visible)
    {
        m_referenceHidden.resize(std::max(m_referenceHidden.size(), qsizetype(index) + 1));
        m_referenceHidden[index] = !visible;
        fitView();
    }

    // A newly loaded run's dashed curves are all shown.
    void showAllReferences() { m_referenceHidden.clear(); }

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
            if (!m_referenceHidden.value(i)) {
                painter.setPen(QPen(m_referenceColors.value(i), 2, Qt::DashLine));
                painter.drawPolyline(polyline(m_references.at(i), plot));
            }
        }
        drawCursor(painter, plot, QColor(255, 0, 0, 180));
    }

  private:
    qreal toPixelY(qreal y, const QRectF &plot) const
    {
        return plot.bottom() - (y - yMinimum()) / std::max(1e-6, yMaximum() - yMinimum()) * plot.height();
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
        for (int i = 0; i < m_references.size(); ++i) {
            if (!m_referenceHidden.value(i)) {
                take(m_references.at(i));
            }
        }
        qreal yLow = -1, yHigh = 1; // the plot's range while it has no data
        if (low <= high) {
            const qreal padding = std::max(0.2, std::max(1e-6, high - low) * 0.12);
            yLow = low - padding;
            yHigh = high + padding;
            // As the plot's own auto range: no negative axis for readings that are never negative.
            if (low >= 0) {
                yLow = std::max<qreal>(0, yLow);
            } else if (high <= 0) {
                yHigh = std::min<qreal>(0, yHigh);
            }
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
    QList<bool> m_referenceHidden; // dashed curves hidden from the header
    bool m_autoView = true;
    bool m_fitting = false;
    qreal m_fitted[4] = {};
};

// The row above the plot: the chart's title, then an entry per curve reading "name value", the dashed comparison
// curves' too, flowing left to right and onto further rows only where they do not fit. An entry keeps the widest it
// has been, so a reading that gets shorter (100 -> 99.82) leaves the entries after it where they are; a new one
// starts wide enough for a typical reading. Clicking an entry calls toggled(index). PyQt UI Designer's ChartHeader.
class ChartHeader : public QWidget
{
  public:
    struct Entry
    {
        QString name;
        QString value;
        QColor color;
        bool dashed = false; // a loaded run's curve
        bool shown = true;
    };

    explicit ChartHeader(QWidget *parent) : QWidget(parent)
    {
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    std::function<void(int index)> toggled;

    void setTitle(const QString &title)
    {
        m_title = title;
        updateGeometry();
        update();
    }

    void setForeground(const QColor &color)
    {
        m_foreground = color;
        update();
    }

    void setEntries(const QList<Entry> &entries)
    {
        const QFontMetricsF metrics(entryFont());
        const int height = heightForWidth(width());
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if (i == m_widths.size()) {
                m_widths.append(metrics.horizontalAdvance(entries.at(i).name + QLatin1Char(' ') +
                                                          QLatin1String(kSampleReading)));
            }
            m_widths[i] = std::max(m_widths.at(i), metrics.horizontalAdvance(text(entries.at(i))));
        }
        m_entries = entries;
        if (heightForWidth(width()) != height) {
            updateGeometry();
        }
        update();
    }

    // The entries from `first` on belong to new curves (rebuilt, or a run loaded or cleared).
    void renew(int first)
    {
        m_widths.resize(std::clamp<qsizetype>(first, 0, m_widths.size()));
        updateGeometry();
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return places(width).height; }

    QSize sizeHint() const override
    {
        const Places all = places(1 << 20); // all in one row
        qreal right = all.title.right();
        for (const QRectF &rect : all.entries) {
            right = std::max(right, rect.right());
        }
        return QSize(int(std::ceil(right)) + kPad, heightForWidth(width()));
    }

    QSize minimumSizeHint() const override { return QSize(0, heightForWidth(width())); }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        const Places at = places(width());
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!m_title.isEmpty()) {
            painter.setFont(titleFont());
            painter.setPen(m_foreground);
            painter.drawText(at.title, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip, m_title);
        }
        painter.setFont(entryFont());
        for (int i = 0; i < int(at.entries.size()); ++i) {
            const Entry &entry = m_entries.at(i);
            const QRectF rect = at.entries.at(i);
            QColor sample = entry.shown ? entry.color : m_foreground;
            QColor label = m_foreground;
            if (!entry.shown) { // a hidden curve's entry is greyed
                sample.setAlpha(82);
                label.setAlpha(86);
            }
            const qreal middle = rect.center().y();
            painter.setPen(QPen(sample, 2, entry.dashed ? Qt::DashLine : Qt::SolidLine, Qt::FlatCap));
            painter.drawLine(QPointF(rect.left(), middle), QPointF(rect.left() + kSample, middle));
            painter.setPen(label);
            painter.drawText(rect.adjusted(kText, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip,
                             text(entry));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const QList<QRectF> entries = places(width()).entries;
            for (int i = 0; i < int(entries.size()); ++i) {
                if (entries.at(i).contains(event->position()) && toggled) {
                    toggled(i);
                    break;
                }
            }
        }
        event->accept();
    }

  private:
    static constexpr int kPad = 8, kGap = 14, kTitleGap = 20, kSample = 20, kText = 26;

    struct Places
    {
        QRectF title;
        QList<QRectF> entries;
        int height = 0;
    };

    static QString text(const Entry &entry) { return entry.name + QLatin1Char(' ') + entry.value; }

    QFont entryFont() const
    {
        QFont entry = font();
        entry.setPointSizeF(9);
        return entry;
    }

    QFont titleFont() const
    {
        QFont title = font();
        title.setPointSizeF(10);
        return title;
    }

    // The title's rect, the entries' rects and the height, for a width.
    Places places(int width) const
    {
        const qreal row =
            std::ceil(std::max(QFontMetricsF(entryFont()).height(), QFontMetricsF(titleFont()).height())) + 6;
        QList<qreal> widths;
        if (!m_title.isEmpty()) {
            widths.append(QFontMetricsF(titleFont()).horizontalAdvance(m_title));
        }
        for (qsizetype i = 0; i < m_entries.size() && i < m_widths.size(); ++i) {
            widths.append(kText + m_widths.at(i));
        }
        Places at;
        qreal x = kPad, y = 2;
        QList<QRectF> rects;
        for (qsizetype i = 0; i < widths.size(); ++i) {
            if (x > kPad && x + widths.at(i) > width - kPad) {
                x = kPad;
                y += row;
            }
            rects.append(QRectF(x, y, widths.at(i), row));
            x += widths.at(i) + (!m_title.isEmpty() && i == 0 ? kTitleGap : kGap);
        }
        if (!m_title.isEmpty()) {
            at.title = rects.takeFirst();
        }
        at.entries = rects;
        at.height = widths.isEmpty() ? 0 : int(y + row + 2);
        return at;
    }

    QString m_title;
    QColor m_foreground = QColor(0x20, 0x20, 0x20);
    QList<Entry> m_entries;
    QList<qreal> m_widths; // the widest each entry's text has been
};

RealtimeChartWidget::RealtimeChartWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(2);

    // The title and a "name value" entry per curve, the dashed ones too; clicking one hides or shows its curve.
    // Above the plot, so no curve runs under it.
    m_header = new ChartHeader(this);
    m_header->toggled = [this](int index) { toggleCurve(index); };
    root->addWidget(m_header);
    m_plot = new ChartPlot(this);
    root->addWidget(m_plot, 1);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(4, 2, 4, 0);
    toolbar->setSpacing(6);
    toolbar->addWidget(new FluentQt::BodyLabel(tr("显示点数"), this));
    m_displayEdit = new FluentQt::LineEdit(this);
    m_displayEdit->setObjectName(QStringLiteral("chartDisplayPoints"));
    m_displayEdit->setAccessibleName(tr("显示点数"));
    m_displayEdit->setFixedWidth(80);
    toolbar->addWidget(m_displayEdit);
    // 实时, the cursor's sample, or what went wrong saving, loading or recording.
    m_mode = new FluentQt::CaptionLabel(tr("实时"), this);
    m_mode->setTextColor(QColor(QStringLiteral("#0078d4")), QColor(QStringLiteral("#4aa3df")));
    m_mode->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_mode->setMinimumWidth(1); // a long message is cut short rather than widening the chart
    toolbar->addWidget(m_mode);
    toolbar->addStretch();
    auto *save = new FluentQt::ToolButton(FluentQt::icon(FluentQt::FluentIcon::Save), this);
    fluentToolTip(save, tr("保存完整曲线数据"));
    auto *load = new FluentQt::ToolButton(FluentQt::icon(FluentQt::FluentIcon::Folder), this);
    fluentToolTip(load, tr("加载对比曲线"));
    m_clearReference = new FluentQt::ToolButton(FluentQt::icon(FluentQt::FluentIcon::Broom), this);
    fluentToolTip(m_clearReference, tr("清除对比"));
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
        setDisplayPoints(ok ? typed : m_displayPoints);
        if (ok)
            saveViewPreference(QStringLiteral("displayPoints"), m_displayPoints);
        refresh();
    });
    connect(save, &QToolButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(visibleWindow(this), tr("保存完整曲线数据"), QString(),
                                                          tr("CSV文件 (*.csv);;JSON文件 (*.json)"));
        if (!path.isEmpty()) {
            saveHistory(path);
        }
    });
    connect(load, &QToolButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(visibleWindow(this), tr("加载对比曲线"), QString(),
                                                          tr("CSV文件 (*.csv);;JSON文件 (*.json)"));
        if (!path.isEmpty()) {
            loadReference(path);
        }
    });
    connect(m_clearReference, &QToolButton::clicked, this, [this]() {
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
    const int count = chartCurveCount(u"RealtimeChart", props);
    std::vector<Curve> curves;
    for (int i = 0; i < count; ++i) {
        const QJsonObject binding = chartCurveBinding(props, i);
        auto entry = BindingEntry::fromWidget(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("RealtimeChart")}, {QStringLiteral("binding"), binding}});
        curves.push_back({chartCurveName(props, i), std::move(entry), {}});
    }
    return curves;
}

void RealtimeChartWidget::apply()
{
    m_header->setTitle(text("title"));
    setDisplayPoints(integer("displayPoints"));
    m_curves = curvesFromProperties(properties());
    rebuildCurves();
}

void RealtimeChartWidget::setDisplayPoints(int points)
{
    m_displayPoints = std::clamp(points, 10, kHistoryLimit);
    m_displayEdit->setText(QString::number(m_displayPoints));
    m_plot->setCapacity(m_displayPoints);
}

void RealtimeChartWidget::rebuildCurves()
{
    clearReference(); // its header entries follow the live curves', rebuilt here
    // A curve keeps its samples while the curves are reconfigured; only removed curves lose theirs.
    while (m_histories.size() > m_curves.size()) {
        m_histories.pop_back();
    }
    while (m_histories.size() < m_curves.size()) {
        m_histories.push_back(std::make_unique<ChartHistory>(kHistoryLimit));
    }
    m_plot->clearSeries();
    for (size_t i = 0; i < m_curves.size(); ++i) {
        m_plot->addSeries(m_curves[i].name, chartSeriesColor(int(i)));
    }
    m_header->renew(0);
    m_cursorMode = false;
    m_plot->hideCursor();
    m_mode->setText(tr("实时"));
    applyTheme();
}

void RealtimeChartWidget::applyTheme()
{
    const bool dark = isDarkTheme();
    const QString foreground = dark ? QStringLiteral("#e5e5e5") : QStringLiteral("#202020");
    m_header->setForeground(QColor(foreground));
    for (int i = 0; i < m_plot->seriesCount(); ++i) {
        m_plot->setSeriesColor(i, chartSeriesColor(i));
    }
    refresh(); // and the header's entries in the new curve colours
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
        colors.append(chartSeriesColor(int(m_curves.size()) + i));
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
    QStringList values;
    for (size_t i = 0; i < m_curves.size(); ++i) {
        const std::optional<double> last = m_histories[i]->last();
        const auto quality = m_curves[i].quality;
        values.append(quality != DataQuality::Current && quality != DataQuality::Waiting
                          ? dataQualityText(quality) : last ? reading(*last) : QStringLiteral("--"));
        count = std::max(count, m_histories[i]->count());
    }
    showValues(values, count - 1); // the loaded run where the live one has got to
}

void RealtimeChartWidget::receiveCurve(int index, const QVariant &value, const DataSource &source)
{
    auto &curve = m_curves[size_t(index)];
    if (const auto y = curve.binding->chartSample(value)) {
        curve.source.update({*y, source, {}, DataQuality::Current, {}});
        updateCurve(index, curve.source.sample());
    }
}

void RealtimeChartWidget::updateCurve(int index, const DataPointSample &sample)
{
    auto &curve = m_curves[size_t(index)];
    const bool changed = curve.quality != sample.quality;
    curve.quality = sample.quality;
    if (sample.quality == DataQuality::Current)
        appendSample(index, sample.value.toDouble());
    else if (changed && !m_refreshTimer->isActive())
        m_refreshTimer->start();
}

void RealtimeChartWidget::showCursorValues()
{
    // Python's round(): halves go to the even neighbour.
    const qint64 index = qint64(std::nearbyint(m_cursorX));
    m_mode->setText(tr("游标: x=%1").arg(index));
    QStringList values;
    for (size_t i = 0; i < m_curves.size(); ++i) {
        const QList<double> shown = m_histories[i]->tail(m_displayPoints);
        values.append(index >= 0 && index < shown.size() ? reading(shown.at(index)) : QStringLiteral("--"));
    }
    showValues(values, windowStart() + index);
}

void RealtimeChartWidget::showValues(const QStringList &live, qint64 referenceSample)
{
    QList<ChartHeader::Entry> entries;
    for (int i = 0; i < live.size(); ++i) {
        entries.append({m_curves[size_t(i)].name, live.at(i), chartSeriesColor(i), false, m_plot->isSeriesVisible(i)});
    }
    for (int i = 0; i < m_references.size(); ++i) {
        const QList<double> &values = m_references.at(i).values;
        const QString value = referenceSample >= 0 && referenceSample < values.size()
                                  ? reading(values.at(referenceSample))
                                  : QStringLiteral("--");
        entries.append({m_references.at(i).name, value, chartSeriesColor(int(m_curves.size()) + i), true,
                        m_plot->isReferenceVisible(i)});
    }
    m_header->setEntries(entries);
}

void RealtimeChartWidget::toggleCurve(int index)
{
    if (index < m_plot->seriesCount()) {
        const bool shown = !m_plot->isSeriesVisible(index);
        m_plot->setSeriesVisible(index, shown);
        saveViewPreference(QStringLiteral("visibleCurve%1").arg(index), shown);
    } else {
        const int reference = index - m_plot->seriesCount();
        m_plot->setReferenceVisible(reference, !m_plot->isReferenceVisible(reference));
    }
    refresh();
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
        m_references.append({tr("%1（%2）").arg(names.at(i), stem), values.value(i)});
    }
    m_clearReference->show();
    applyTheme();
}

void RealtimeChartWidget::clearReference()
{
    m_references.clear();
    m_plot->showAllReferences();
    m_header->renew(int(m_curves.size()));
    m_clearReference->hide();
}

void RealtimeChartWidget::onAttached()
{
    setDisplayPoints(viewPreference(QStringLiteral("displayPoints"), m_displayPoints).toInt());
    for (int i = 0; i < m_plot->seriesCount(); ++i) {
        const bool shown = viewPreference(QStringLiteral("visibleCurve%1").arg(i), true).toBool();
        m_plot->setSeriesVisible(i, shown);
    }
    refresh();
    m_curves = curvesFromProperties(properties());
    for (Curve &curve : m_curves) {
        if (curve.binding && !context()->resolveBindingSource(*curve.binding)) {
            curve.quality = DataQuality::ConfigurationError;
            curve.binding.reset();
        }
    }
    m_hasCanCurves = std::any_of(m_curves.begin(), m_curves.end(), [](const Curve &curve) {
        return curve.binding && curve.binding->parseMode == ParseMode::CanSignal;
    });

    live(connect(context()->projectData(), &ProjectData::valuesChanged, this, [this](const Message &values) {
        if (!context()->isActive())
            return;
        for (size_t i = 0; i < m_curves.size(); ++i) {
            const auto &binding = m_curves[i].binding;
            if (!binding || binding->parseMode != ParseMode::DataPoint || !values.contains(binding->key))
                continue;
            auto sample = context()->projectData()->sample(binding->key);
            if (sample.quality == DataQuality::Current) {
                const auto y = binding->chartSample(values.value(binding->key));
                if (!y)
                    continue;
                sample.value = *y;
            }
            updateCurve(int(i), sample);
        }
    }));
    QList<S7::Need> s7Needs;
    for (const Curve &curve : m_curves) {
        if (curve.binding && curve.binding->s7)
            s7Needs.append({curve.binding->sourceCardId, *curve.binding->s7});
    }
    context()->setS7CurveNeeds(this,s7Needs);
    live(connect(context(),&RuntimeContext::s7Sample,this,[this](const S7::Sample &sample) {
        for(size_t i=0;i<m_curves.size();++i) {
            const auto &binding=m_curves[i].binding;
            if (binding && binding->s7 && binding->sourceCardId == sample.sourceCardId &&
                binding->s7->address.canonical == sample.address && binding->s7->type.name == sample.dataType) {
                if (sample.quality == QLatin1String("good"))
                    receiveCurve(int(i), sample.value, {sample.sourceCardId, 0, {}});
                else
                    updateCurve(int(i), {{}, {}, {}, DataQuality::Old, {}});
            }
        }
    }));

    // Modbus curves are read by automatic polling (ModbusPoller), at its default period.
    QList<ModbusNeed> needs;
    for (const Curve &curve : m_curves) {
        if (curve.binding)
            if (const auto need = modbusNeed(*curve.binding))
                needs.append(*need);
    }
    context()->setCurveNeeds(this, needs);
    CommHub *hub = context()->hub();
    live(connect(hub, &CommHub::messageReceived, this, [this](const Message &message, const DataSource &source) {
        if (!context()->isActive()) {
            return;
        }
        for (size_t i = 0; i < m_curves.size(); ++i) {
            const auto &binding = m_curves[i].binding;
            if (!binding || !context()->acceptsMessage(*binding, message, source)) {
                continue;
            }
            // A malformed sample skips this curve only; the frame's other curves still update.
            receiveCurve(int(i), binding->extractFromParsed(message), source);
        }
    }));
    live(connect(context(), &RuntimeContext::modbusReadUnavailable, this,
        [this](const Link &link, const QString &endpoint, const ModbusBlock &block, int fallbackSlave) {
            for (size_t index = 0; index < m_curves.size(); ++index) {
                auto &curve = m_curves[index];
                const auto source = curve.source.sample().source;
                if (curve.binding && RuntimeContext::matchesModbusRead(*curve.binding, source, link, endpoint, block, fallbackSlave) &&
                    curve.source.markOld(source.stream))
                    updateCurve(int(index), curve.source.sample());
            }
        }));
    live(connect(hub, &CommHub::rawReceived, this, [this](const QByteArray &bytes, const DataSource &source) {
        if (!context()->isActive()) {
            return;
        }
        for (size_t i = 0; i < m_curves.size(); ++i) {
            auto &binding = m_curves[i].binding;
            if (!binding || !binding->usesRawData() || source.canText || !binding->route().accepts(source)) {
                continue;
            }
            for (const QVariant &value : binding->extractAllFromRaw(bytes, source.stream))
                receiveCurve(int(i), value, source);
        }
    }));
    live(connect(hub, &CommHub::canFrames, this, [this](CanLink *, const QList<CanFrame> &frames, bool sent, const DataSource &source) {
        // Only what the bus brought in, as for a control's CAN信号 binding.
        if (!m_hasCanCurves || sent || !context()->isActive()) {
            return;
        }
        for (const CanFrame &frame : frames) {
            for (size_t i = 0; i < m_curves.size(); ++i) {
                const auto &binding = m_curves[i].binding;
                if (!binding || binding->parseMode != ParseMode::CanSignal || !binding->route().accepts(source)) {
                    continue;
                }
                receiveCurve(int(i), binding->extractFromCan(frame), source);
            }
        }
    }));
    live(connect(hub, &CommHub::streamClosed, this, [this](const DataSource &source) {
        for (size_t i = 0; i < m_curves.size(); ++i) {
            auto &curve = m_curves[i];
            if (curve.binding)
                curve.binding->resetStream(source.stream);
            if (curve.source.close(source.stream))
                updateCurve(int(i), curve.source.sample());
        }
    }));
    live(connect(context(), &RuntimeContext::activeChanged, this, [this](bool active) {
        if (active)
            return;
        for (auto &curve : m_curves) {
            curve.source = {};
            if (curve.binding)
                curve.binding->resetStream();
        }
    }));
}

void RealtimeChartWidget::onDetached()
{
    context()->setCurveNeeds(this, {});
    context()->setS7CurveNeeds(this, {});
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
    m_recordHint = new FluentQt::BodyLabel(tr("请在设计器配置逐行记录的源字段和列标题"), this);
    m_recordHint->setAlignment(Qt::AlignCenter);
    m_recordHint->setWordWrap(true);
    layout->addWidget(m_recordHint, 1);
    m_recordTimer = new QTimer(this);
}

bool DataTableWidget::logging() const
{
    return text("recordMode") == QStringLiteral("逐行记录"); // i18n: value
}

bool DataTableWidget::loggingPoints() const
{
    return logging() && text("recordSource") == QLatin1String("points");
}

void DataTableWidget::apply()
{
    m_recordFields.clear();
    QStringList headers;
    const bool records = logging();
    if (records) {
        for (const QJsonValue &entry : prop("recordColumns").toArray()) {
            const QJsonObject column = entry.toObject();
            const QString field = column.value(QStringLiteral("field")).toString().trimmed();
            if (field.isEmpty())
                continue;
            m_recordFields.append(field);
            const QString label = column.value(QStringLiteral("label")).toString().trimmed();
            headers.append(label.isEmpty() ? (field == kTimeField ? tr("时间") : field) : label);
        }
    } else {
        headers = {tr("字段"), tr("值")};
    }
    const bool hasSourceField = std::any_of(m_recordFields.cbegin(), m_recordFields.cend(),
                                            [](const QString &field) { return field != kTimeField; });
    m_table->setVisible(!records || hasSourceField);
    m_recordHint->setVisible(records && !hasSourceField);
    m_table->setRowCount(0);
    m_table->setColumnCount(int(headers.size()));
    m_table->setHorizontalHeaderLabels(headers);
    m_table->verticalHeader()->setVisible(!records);
    QHeaderView *header = m_table->horizontalHeader();
    for (int c = 0; c < headers.size(); ++c) {
        header->setSectionResizeMode(c, records && c < headers.size() - 1 ? QHeaderView::ResizeToContents
                                                                         : QHeaderView::Stretch);
    }
}

void DataTableWidget::onAttached()
{
    if (loggingPoints()) {
        auto *data = context()->projectData();
        const int interval = integer("recordIntervalMs");
        if (interval > 0) {
            live(connect(m_recordTimer, &QTimer::timeout, this, [this, data] {
                if (context()->isActive())
                    appendRecord(data->values());
            }));
            m_recordTimer->start(interval);
        } else {
            const QString trigger = text("recordTriggerPoint");
            live(connect(data, &ProjectData::valuesChanged, this, [this, data, trigger](const Message &changes) {
                const bool updated = trigger.isEmpty()
                    ? std::any_of(m_recordFields.cbegin(), m_recordFields.cend(),
                                  [&changes](const QString &field) { return changes.value(field).isValid(); })
                    : changes.value(trigger).isValid();
                if (updated && context()->isActive())
                    appendRecord(data->values());
            }));
        }
        return;
    }
    live(connect(context()->hub(), &CommHub::messageReceived, this, &DataTableWidget::onMessage));
}

void DataTableWidget::onDetached()
{
    m_recordTimer->stop();
}

void DataTableWidget::appendRecord(const Message &values)
{
    if (m_recordFields.isEmpty() || !m_recordHint->isHidden())
        return;
    if (m_table->rowCount() >= std::max(10, integer("maxRows")))
        m_table->removeRow(0);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    for (int column = 0; column < m_recordFields.size(); ++column) {
        const QString &field = m_recordFields.at(column);
        const auto value = values.value(field);
        const QString cell = field == kTimeField ? stamp : value.isValid() ? displayString(value) : QString();
        m_table->setItem(row, column, new QTableWidgetItem(cell));
    }
    m_table->scrollToBottom();
}

void DataTableWidget::onMessage(const Message &message, const DataSource &source)
{
    if (!context() || !context()->isActive() || !receiveRoute().accepts(source)) {
        return;
    }
    if (logging()) {
        // A message that contains none of the configured fields belongs to another record.
        if (std::none_of(m_recordFields.cbegin(), m_recordFields.cend(), [&message](const QString &field) {
                return field != kTimeField && message.contains(field);
            })) {
            return;
        }
        appendRecord(message);
        return;
    }
    m_table->setRowCount(0);
    for (auto it = message.constBegin(); it != message.constEnd(); ++it) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(it.key()));
        m_table->setItem(row, 1, new QTableWidgetItem(displayString(it.value())));
    }
}

// ---------------------------------------------------------------------------
// ModbusSlaveTable
// ---------------------------------------------------------------------------

ModbusSlaveTableWidget::ModbusSlaveTableWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    m_table = new FluentQt::TableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({tr("地址"), tr("值")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table);
    connect(m_table, &QTableWidget::itemChanged, this, &ModbusSlaveTableWidget::edited);
}

QString ModbusSlaveTableWidget::table() const
{
    const QString prefix = ModbusArea::prefix(text("mbArea"));
    return prefix.isEmpty() ? QStringLiteral("hr") : prefix;
}

const ModbusSlaveStore *ModbusSlaveTableWidget::store() const
{
    if (isLive()) {
        return m_slave ? &m_slave->store() : nullptr;
    }
    return m_design ? &*m_design : nullptr;
}

void ModbusSlaveTableWidget::apply()
{
    rebuild();
}

void ModbusSlaveTableWidget::showDesign(const QJsonObject *cardProperties, bool ambiguous, bool editInitialValues)
{
    m_ambiguousSource = ambiguous;
    m_editInitialValues = editInitialValues;
    if (cardProperties) {
        m_design = ModbusSlaveStore::fromProperties(*cardProperties);
    } else {
        m_design.reset();
    }
    rebuild();
}

QString ModbusSlaveTableWidget::initialValues() const
{
    return m_design ? m_design->initialValues() : QString();
}

void ModbusSlaveTableWidget::onAttached()
{
    // A card later in the document attaches after this table: take its slave once it is there.
    live(connect(context()->hub(), &CommHub::slavesChanged, this, &ModbusSlaveTableWidget::bindSlave));
    bindSlave();
}

void ModbusSlaveTableWidget::onDetached()
{
    disconnect(m_slaveConnection);
    m_slave.clear();
    m_ambiguousSource = false;
    rebuild();
}

void ModbusSlaveTableWidget::bindSlave()
{
    const QList<ModbusSlave *> slaves = context()->hub()->modbusSlaves(receiveRoute());
    m_ambiguousSource = slaves.size() > 1;
    ModbusSlave *first = slaves.size() == 1 ? slaves.first() : nullptr;
    if (first == m_slave && m_slave) {
        return;
    }
    disconnect(m_slaveConnection);
    m_slave = first;
    if (first) {
        m_slaveConnection = connect(first, &ModbusSlave::changed, this, &ModbusSlaveTableWidget::showChange);
    }
    rebuild();
}

QString ModbusSlaveTableWidget::display(quint16 value) const
{
    const QString t = table();
    if (t == QLatin1String("co") || t == QLatin1String("di")) {
        return value ? QStringLiteral("1") : QStringLiteral("0");
    }
    const QString format = text("displayFormat");
    if (format == QStringLiteral("有符号")) { // i18n: value
        return QString::number(qint16(value));
    }
    if (format == QStringLiteral("十六进制")) { // i18n: value
        return QStringLiteral("0x") + QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0')).toUpper();
    }
    return QString::number(value);
}

std::optional<quint16> ModbusSlaveTableWidget::parse(const QString &typed) const
{
    QString value = typed.trimmed();
    const QString t = table();
    bool ok = false;
    if (t == QLatin1String("co") || t == QLatin1String("di")) {
        if (value == QLatin1String("0") || value == QLatin1String("1")) {
            return quint16(value.toInt());
        }
        return std::nullopt;
    }
    const QString format = text("displayFormat");
    if (format == QStringLiteral("十六进制")) { // i18n: value
        if (value.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
            value = value.mid(2);
        }
        static const QRegularExpression digits(QStringLiteral("^[0-9A-Fa-f]{1,4}$"));
        if (!digits.match(value).hasMatch()) {
            return std::nullopt;
        }
        return quint16(value.toUInt(&ok, 16));
    }
    static const QRegularExpression number(QStringLiteral("^[+-]?[0-9]+$"));
    if (!number.match(value).hasMatch()) {
        return std::nullopt;
    }
    const qlonglong n = value.toLongLong(&ok);
    const bool signedFormat = format == QStringLiteral("有符号"); // i18n: value
    if (!ok || n < (signedFormat ? -32768 : 0) || n > (signedFormat ? 32767 : 65535)) {
        return std::nullopt;
    }
    return quint16(n & 0xFFFF);
}

void ModbusSlaveTableWidget::rebuild()
{
    m_filling = true;
    m_table->clearSpans();
    m_table->setRowCount(0);
    const ModbusSlaveStore *source = store();
    if (!source) {
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setRowCount(1);
        auto *hint = new QTableWidgetItem(m_ambiguousSource ? tr("请选择一个从站连接") : tr("没有从站链路卡片"));
        hint->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(0, 0, hint);
        m_table->setSpan(0, 0, 1, 2);
        m_filling = false;
        return;
    }
    m_table->setEditTriggers(isLive() || m_editInitialValues
        ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed : QAbstractItemView::NoEditTriggers);
    const QString t = table();
    const ModbusSlaveStore::Area area = source->area(t);
    m_table->setRowCount(area.count);
    for (int row = 0; row < area.count; ++row) {
        auto *address = new QTableWidgetItem(QString::number(area.start + row));
        address->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_table->setItem(row, 0, address);
        auto *value = new QTableWidgetItem(display(source->value(t, area.start + row)));
        value->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable);
        m_table->setItem(row, 1, value);
    }
    m_filling = false;
}

void ModbusSlaveTableWidget::showChange(const Message &message)
{
    const ModbusSlaveStore *source = store();
    const QString t = table();
    if (!source || message.value(QStringLiteral("fc")).toInt() != ModbusSlaveStore::readFunction(t)) {
        return;
    }
    const int start = source->area(t).start;
    m_filling = true;
    for (auto it = message.constBegin(); it != message.constEnd(); ++it) {
        if (!it.key().startsWith(t)) {
            continue;
        }
        bool ok = false;
        const int address = it.key().mid(t.size()).toInt(&ok);
        if (QTableWidgetItem *item = ok ? m_table->item(address - start, 1) : nullptr) {
            item->setText(display(source->value(t, address)));
        }
    }
    m_filling = false;
}

void ModbusSlaveTableWidget::edited(QTableWidgetItem *item)
{
    const auto *current = store();
    if (m_filling || !current || item->column() != 1 || (!isLive() && !m_editInitialValues)) {
        return;
    }
    const QString t = table();
    const int address = current->area(t).start + item->row();
    const std::optional<quint16> value = parse(item->text());
    if (value && m_editInitialValues && !isLive()) {
        m_design->write(t, address, {*value});
        return;
    }
    if (!value || !m_slave->write(t, address, {*value})) {
        m_filling = true;
        item->setText(display(current->value(t, address)));
        m_filling = false;
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
    const auto event = [this](const QString &message, const QString &level, const DataSource &source) {
        if (acceptsEvent(receiveRoute(), source))
            appendLog(withSource(message, source), level);
    };
    live(connect(hub, &CommHub::linkConnected, this,
                 [event](const QString &link, const DataSource &source) {
                     event(tr("已连接: %1").arg(link), QStringLiteral("ok"), source);
                 }));
    live(connect(hub, &CommHub::linkDisconnected, this,
                 [event](const QString &link, const DataSource &source) {
                     event(tr("已断开: %1").arg(link), QStringLiteral("warn"), source);
                 }));
    live(connect(hub, &CommHub::linkError, this,
                 [event](const QString &message, const DataSource &source) {
                     event(tr("错误: %1").arg(message), QStringLiteral("error"), source);
                 }));
    live(connect(hub, &CommHub::linkWarning, this,
                 [event](const QString &message, const DataSource &source) {
                     event(tr("警告: %1").arg(message), QStringLiteral("warn"), source);
                 }));
    live(connect(hub, &CommHub::linkInfo, this, [event](const QString &message, const DataSource &source) {
        event(message, QStringLiteral("info"), source);
    }));
    live(connect(hub, &CommHub::messageReceived, this, [this](const Message &message, const DataSource &source) {
        // A slave's own change is no frame received: its requests and replies are logged instead.
        if (context()->isActive() && !source.slaveData && receiveRoute().accepts(source)) {
            appendLog(withSource(messageText(message), source));
        }
    }));
    live(connect(hub, &CommHub::slaveLog, this, event));
}

// ---------------------------------------------------------------------------
// DataRecvCard
// ---------------------------------------------------------------------------

namespace {

const QColor receivedColor(0xa6e3a1);
const QColor sentColor(0xf9e2af);
const QColor eventColor(0x74c7ec);
constexpr int kReceiveLogRefreshMs = 40;
constexpr std::size_t kReceiveLogBatchRows = 512;

QString singleLogLine(const QString &text)
{
    QString line;
    line.reserve(text.size());
    for (const QChar c : text) {
        if (c == u'\r')
            line += QStringLiteral("\\r");
        else if (c == u'\n')
            line += QStringLiteral("\\n");
        else if (c == u'\t')
            line += QStringLiteral("\\t");
        else if (c.category() == QChar::Other_Control || c == QChar::LineSeparator || c == QChar::ParagraphSeparator)
            line += QStringLiteral("\\x%1").arg(int(c.unicode()), 2, 16, u'0');
        else
            line += c;
    }
    return line;
}

void writeReceiveLog(QTextCursor &cursor, const QTime &time, const QString &kind, const QString &text,
                     const DataSource &source, const QColor &color)
{
    QTextCharFormat muted;
    muted.setForeground(QColor(0x9399b2));
    QTextCharFormat body;
    body.setForeground(color);
    cursor.insertText(time.toString(QStringLiteral("HH:mm:ss.zzz")) + QStringLiteral("  "), muted);
    cursor.insertText(kind + QStringLiteral("  ") + singleLogLine(text), body);
    const auto label = withSource({}, source).trimmed();
    if (!label.isEmpty())
        cursor.insertText(QStringLiteral("  ") + singleLogLine(label), muted);
}

template <typename Update>
void updatePacketLog(QTextEdit *output, bool autoScroll, Update update)
{
    // Changing the document can move its cursor without the user selecting a row.
    const QSignalBlocker editing(output);
    auto *vertical = output->verticalScrollBar();
    auto *horizontal = output->horizontalScrollBar();
    const int y = vertical->value();
    const int x = horizontal->value();
    if (autoScroll && y == vertical->maximum()) {
        update();
        vertical->setValue(vertical->maximum());
    } else {
        auto anchor = output->cursorForPosition(QPoint(0, 0));
        anchor.setKeepPositionOnInsert(true);
        const auto *layout = output->document()->documentLayout();
        const auto before = layout->blockBoundingRect(anchor.block()).top();
        update();
        // The document can drop old blocks when it reaches the configured line limit.
        vertical->setValue(y + qRound(layout->blockBoundingRect(anchor.block()).top() - before));
    }
    horizontal->setValue(x);
}

} // namespace

struct DataRecvCardWidget::PacketRecord : QTextBlockUserData
{
    PacketRecord(const QString &kind, const DataSource &source, const QColor &color, const QString &text = {})
        : kind(kind), source(source), color(color), text(text)
    {
    }

    QTime time = QTime::currentTime();
    QString kind;
    DataSource source;
    QColor color;
    QString text;
    // Raw packet rows retain their bytes so changing the display format can redraw them.
    QByteArray bytes;
    bool received = false;
    bool binary = false;
    std::optional<ReceiveSample> sample;
};

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
    auto *options = new FluentQt::FlowLayout(0, 6, 4);
    options->setVerticalAlignment(Qt::AlignVCenter);
    m_format = new FluentQt::ComboBox(this);
    m_format->setObjectName(QStringLiteral("receiveLogFormat"));
    m_format->setAccessibleName(tr("报文显示格式"));
    m_format->addItem(tr("自动显示"), {}, QStringLiteral("auto"));
    m_format->addItem(QStringLiteral("HEX"), {}, QStringLiteral("hex"));
    m_format->addItem(tr("文本"), {}, QStringLiteral("text"));
    m_format->setMinimumWidth(m_format->sizeHint().width());
    m_wire = new FluentQt::CheckBox(tr("线路字节"), this);
    m_wire->setObjectName(QStringLiteral("showWireBytes"));
    m_autoScroll = new FluentQt::CheckBox(tr("自动滚动"), this);
    m_autoScroll->setObjectName(QStringLiteral("receiveLogAutoScroll"));
    m_autoScroll->setChecked(true);
    auto *clear = new FluentQt::PushButton(this);
    clear->setObjectName(QStringLiteral("clearReceiveLog"));
    clear->setText(tr("清空"));
    m_sample = new FluentQt::PushButton(tr("试解析"), this);
    m_sample->setObjectName(QStringLiteral("inspectReceivedSample"));
    m_sample->setEnabled(false);
    options->addWidget(m_format);
    options->addWidget(m_wire);
    options->addWidget(m_autoScroll);
    options->addWidget(m_sample);
    options->addWidget(clear);
    m_output = createTerminal(this);
    m_output->setObjectName(QStringLiteral("receivePacketLog"));
    m_output->setLineWrapMode(QTextEdit::NoWrap);
    layout->addLayout(titleRow);
    layout->addLayout(options);
    layout->addWidget(m_output, 1);
    m_logRefresh = new QTimer(this);
    m_logRefresh->setInterval(kReceiveLogRefreshMs);
    m_logRefresh->setTimerType(Qt::PreciseTimer);
    connect(m_logRefresh, &QTimer::timeout, this, &DataRecvCardWidget::flushLog);
    connect(clear, &QPushButton::clicked, this, [this]() {
        m_logRefresh->stop();
        m_pendingLog.clear();
        m_output->clear();
        m_receivedCount = 0;
        m_wireBytes = 0;
        m_recentSample.reset();
        m_selectedRecord = {};
        updateSampleButton();
        refreshCounter();
    });
    connect(m_format, &FluentQt::ComboBox::currentIndexChanged, this, &DataRecvCardWidget::refreshPacketFormat);
    connect(m_wire, &QCheckBox::toggled, this, [this] { refreshCounter(); });
    connect(m_autoScroll, &QCheckBox::toggled, this, [this](bool follows) {
        if (follows)
            m_output->verticalScrollBar()->setValue(m_output->verticalScrollBar()->maximum());
    });
    connect(m_sample, &QPushButton::clicked, this, &DataRecvCardWidget::showReceiveSample);
    connect(m_output, &QTextEdit::cursorPositionChanged, this, [this] {
        if (m_output->hasFocus()) {
            const auto block = m_output->textCursor().block();
            const auto *record = static_cast<const PacketRecord *>(block.userData());
            m_selectedRecord = record && record->sample ? block : QTextBlock{};
            updateSampleButton();
        }
    });
}

DataRecvCardWidget::~DataRecvCardWidget() = default;

void DataRecvCardWidget::apply()
{
    m_title->setText(text("title"));
    refreshCounter();
    m_maxLines = std::max(50, integer("maxLines"));
    while (m_pendingLog.size() > std::size_t(m_maxLines))
        m_pendingLog.pop_front();
    discardEvictedSelection(0);
    const QSignalBlocker editing(m_output);
    m_output->document()->setMaximumBlockCount(m_maxLines);
    updateSampleButton();
}

void DataRecvCardWidget::refreshCounter()
{
    m_counter->setText(m_wire->isChecked() ? tr("线路 %1 B · 收 %2 条").arg(m_wireBytes).arg(m_receivedCount)
                                          : tr("收 %1 条").arg(m_receivedCount));
}

void DataRecvCardWidget::discardEvictedSelection(int incomingRows)
{
    // QTextDocument may reuse a trimmed block's storage for an unrelated log row.
    const int evicted = m_output->document()->blockCount() + incomingRows - m_maxLines;
    if (evicted > 0 && m_selectedRecord.isValid() && m_selectedRecord.blockNumber() < evicted)
        m_selectedRecord = {};
}

const DataRecvCardWidget::ReceiveSample *DataRecvCardWidget::selectedSample() const
{
    if (!m_selectedRecord.isValid())
        return nullptr;
    const auto *record = static_cast<const PacketRecord *>(m_selectedRecord.userData());
    return record && record->sample ? &*record->sample : nullptr;
}

void DataRecvCardWidget::updateSampleButton()
{
    const bool selected = selectedSample() != nullptr;
    m_sample->setText(selected ? tr("试解析所选") : tr("试解析"));
    m_sample->setEnabled(selected || m_recentSample.has_value());
    QList<QTextEdit::ExtraSelection> highlights;
    if (selected) {
        QTextEdit::ExtraSelection row;
        row.cursor = QTextCursor(m_selectedRecord);
        row.format.setProperty(QTextFormat::FullWidthSelection, true);
        row.format.setBackground(QColor(0x343B50));
        highlights.append(row);
    }
    m_output->setExtraSelections(highlights);
}

void DataRecvCardWidget::showReceiveSample()
{
    if (!context())
        return;
    const auto *record = selectedSample();
    if (!record && !m_recentSample)
        return;
    const auto sample = record ? *record : *m_recentSample;
    const auto &settings = sample.format.settings;
    QWidget *host = visibleWindow(this);
    if (sample.can)
        openCanPacketPreview(*sample.can, sample.canDefinitions, sample.source, host, context()->receivePointAction());
    else if (isModbusParser(sample.format.type))
        openModbusPacketPreview(sample.bytes, sample.format.type, sample.request, sample.decoded, sample.source, host,
                                context()->receivePointAction());
    else if (sample.format.type == LinkParser::HexGroup)
        openReceiveGroupPreview(settings, host, {}, sample.bytes);
    else if (sample.format.type == QLatin1String("hex"))
        openReceiveFramePreview(settings, host, sample.bytes);
    else if (sample.format.type == QLatin1String("line"))
        openReceiveTextPreview(settings, host,
                               LineParser::decode(sample.bytes, settings.value(QStringLiteral("encoding")).toString()));
}

QString DataRecvCardWidget::packetText(const PacketRecord &packet, QStringView mode) const
{
    // A CAN link's frame line is text that already shows its bytes in HEX.
    if (packet.source.canText)
        return QString::fromUtf8(packet.bytes);
    const bool automatic = mode == QLatin1String("auto");
    if (mode == QLatin1String("hex") || (automatic && packet.binary))
        return toHexString(packet.bytes);
    const auto text = LineParser::decode(packet.bytes, packet.received ? packet.source.encoding : QStringLiteral("utf-8"));
    if (automatic && std::any_of(text.cbegin(), text.cend(), [](QChar c) {
            return c == QChar::ReplacementCharacter ||
                   (c.category() == QChar::Other_Control && c != u'\r' && c != u'\n' && c != u'\t');
        }))
        return toHexString(packet.bytes);
    return text;
}

void DataRecvCardWidget::appendLog(const QString &kind, const QString &text, const DataSource &source,
                                  const QColor &color)
{
    queueLog(std::make_unique<PacketRecord>(kind, source, color, text));
}

void DataRecvCardWidget::appendPacket(const QByteArray &bytes, bool received, const DataSource &source, const Message &decoded)
{
    if (bytes.isEmpty())
        return;
    auto packet = std::make_unique<PacketRecord>(received ? tr("收") : tr("发"), source,
                                                received ? receivedColor : sentColor);
    packet->bytes = bytes;
    packet->received = received;
    const auto *link = context()->hub()->sourceLink(source);
    const QString parser = link ? link->parserType() : QString();
    const bool modbus = isModbusParser(parser);
    packet->binary = LinkParser::isHex(parser) || (modbus && parser != QLatin1String("modbus_ascii"));
    if (received || modbus)
        packet->sample = packetSample(bytes, source, link, received, decoded);
    if (received && packet->sample)
        m_recentSample = packet->sample;
    queueLog(std::move(packet));
}

std::optional<DataRecvCardWidget::ReceiveSample> DataRecvCardWidget::packetSample(const QByteArray &bytes,
    const DataSource &source, const Link *link, bool received, const Message &decoded) const
{
    if (source.canText || bytes.isEmpty() || !link)
        return {};
    const auto parser = link->parserType();
    if (!LinkParser::isHex(parser) && !isModbusParser(parser) && parser != QLatin1String("line"))
        return {};
    return ReceiveSample{bytes.left(65536), {parser, link->parserConfig()}, source,
                         received == link->answersModbus(), decoded, {}, {}};
}

void DataRecvCardWidget::appendSampleLog(const QString &kind, const QString &text, const QByteArray &bytes,
                                        const DataSource &source, const QColor &color)
{
    auto record = std::make_unique<PacketRecord>(kind, source, color, text);
    record->sample = packetSample(bytes, source, context()->hub()->sourceLink(source));
    if (record->sample)
        m_recentSample = record->sample;
    queueLog(std::move(record));
}

void DataRecvCardWidget::queueLog(std::unique_ptr<PacketRecord> record)
{
    m_pendingLog.push_back(std::move(record));
    if (m_pendingLog.size() > std::size_t(m_maxLines))
        m_pendingLog.pop_front();
    scheduleLogRefresh();
}

void DataRecvCardWidget::scheduleLogRefresh()
{
    if (!m_logRefresh->isActive())
        m_logRefresh->start();
}

void DataRecvCardWidget::flushLog()
{
    refreshCounter();
    if (m_pendingLog.empty()) {
        m_logRefresh->stop();
        return;
    }

    // Like the CAN monitor, retain incoming records separately from the displayed rows.
    // Limit each edit so pending traffic cannot monopolize the UI after a burst.
    const auto count = std::min(m_pendingLog.size(), kReceiveLogBatchRows);
    const auto mode = selectedValue(m_format);
    discardEvictedSelection(int(count));
    updatePacketLog(m_output, m_autoScroll->isChecked(), [&] {
        QTextCursor cursor(m_output->document());
        cursor.movePosition(QTextCursor::End);
        cursor.beginEditBlock();
        for (std::size_t i = 0; i < count; ++i) {
            auto record = std::move(m_pendingLog.front());
            m_pendingLog.pop_front();
            if (!cursor.document()->isEmpty())
                cursor.insertBlock();
            const bool rawPacket = !record->bytes.isEmpty();
            writeReceiveLog(cursor, record->time, record->kind, rawPacket ? packetText(*record, mode) : record->text,
                            record->source, record->color);
            if (rawPacket || record->sample)
                cursor.block().setUserData(record.release());
        }
        cursor.endEditBlock();
    });
    updateSampleButton();
}

void DataRecvCardWidget::refreshPacketFormat()
{
    const auto mode = selectedValue(m_format);
    updatePacketLog(m_output, m_autoScroll->isChecked(), [&] {
        QTextCursor cursor(m_output->document());
        cursor.beginEditBlock();
        for (auto block = cursor.document()->firstBlock(); block.isValid(); block = block.next()) {
            const auto *packet = static_cast<const PacketRecord *>(block.userData());
            if (!packet || packet->bytes.isEmpty())
                continue;
            cursor.setPosition(block.position());
            cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
            writeReceiveLog(cursor, packet->time, packet->kind, packetText(*packet, mode), packet->source,
                            packet->color);
        }
        cursor.endEditBlock();
    });
}

void DataRecvCardWidget::onAttached()
{
    rememberViewOption(m_format, QStringLiteral("format"));
    rememberViewOption(m_wire, QStringLiteral("wireBytes"));
    rememberViewOption(m_autoScroll, QStringLiteral("autoScroll"));
    CommHub *hub = context()->hub();
    const auto event = [this](const QString &message, const DataSource &source) {
        if (!acceptsEvent(receiveRoute(), source))
            return;
        appendLog(tr("事件"), message, source, eventColor);
    };
    live(connect(hub, &CommHub::wireReceived, this, [this, hub](const QByteArray &bytes, const DataSource &source) {
        if (!receiveRoute().accepts(source))
            return;
        m_wireBytes += quint64(bytes.size());
        if (m_wire->isChecked()) {
            auto content = tr("%1 B · %2").arg(bytes.size()).arg(toHexString(bytes.left(512)));
            if (const auto *link = hub->sourceLink(source)) {
                const auto progress = link->frameProgress(source.stream);
                if (progress.expected > 0)
                    content.prepend(tr("等待整帧 %1/%2 B · ").arg(progress.received).arg(progress.expected));
            }
            appendSampleLog(tr("线路"), content, bytes, source, eventColor);
        }
    }));
    live(connect(hub, &CommHub::frameRejected, this, [this](const FrameNotice &notice, const DataSource &source) {
        if (!m_wire->isChecked() || !receiveRoute().accepts(source))
            return;
        QString reason;
        switch (notice.reason) {
        case FrameNotice::Reason::Length: reason = tr("帧长超限或不合法"); break;
        case FrameNotice::Reason::Timeout: reason = tr("组帧超时"); break;
        case FrameNotice::Reason::Checksum: reason = tr("校验值不匹配"); break;
        case FrameNotice::Reason::Footer: reason = tr("帧尾不匹配"); break;
        }
        const auto length = notice.expected >= 0 ? tr("已收 %1 B · 声明 %2 B").arg(notice.received).arg(notice.expected)
                                                  : tr("已收 %1 B").arg(notice.received);
        appendSampleLog(tr("丢帧"), reason + QStringLiteral(" · ") + length + QStringLiteral(" · ") +
                        toHexString(notice.sample.left(512)), notice.sample, source, QColor(0xFFAE91));
    }));
    const auto incoming = [this](const QByteArray &bytes, const DataSource &source, const Message &decoded) {
        appendPacket(bytes, true, source, decoded);
        ++m_receivedCount;
        scheduleLogRefresh();
    };
    live(connect(hub, &CommHub::rawReceived, this, [this, hub, incoming](const QByteArray &bytes, const DataSource &source) {
        if (source.canText || !receiveRoute().accepts(source))
            return;
        const auto *link = hub->sourceLink(source);
        if (!link || !isModbusParser(link->parserType()))
            incoming(bytes, source, {});
    }));
    live(connect(hub, &CommHub::unitReceived, this, [this, hub, incoming](const QByteArray &bytes, const Message &decoded,
                                                                       const DataSource &source) {
        if (bytes.isEmpty() || !receiveRoute().accepts(source))
            return;
        const auto *link = hub->sourceLink(source);
        if (link && isModbusParser(link->parserType()))
            incoming(bytes, source, decoded);
    }));
    live(connect(hub, &CommHub::messageReceived, this, [this](const Message &message, const DataSource &source) {
        if (m_wire->isChecked() && !message.isEmpty() && acceptsEvent(receiveRoute(), source))
            appendLog(tr("解析"), messageText(message), source, eventColor);
    }));
    live(connect(hub, &CommHub::canFrames, this, [this](const CanLink *link, const QList<CanFrame> &frames, bool sent,
                                                      const DataSource &source) {
        if (!receiveRoute().accepts(source))
            return;
        const auto definitions = link ? link->signalSet() : nullptr;
        for (const auto &frame : frames) {
            auto packet = std::make_unique<PacketRecord>(sent ? tr("发") : tr("收"), source, sent ? sentColor : receivedColor);
            packet->received = !sent;
            packet->bytes = Can::frameLine(frame).toUtf8();
            packet->sample = ReceiveSample{{}, {}, source, false, {}, frame, definitions};
            if (!sent) {
                m_recentSample = packet->sample;
                m_wireBytes += quint64(frame.data.size());
                ++m_receivedCount;
            }
            queueLog(std::move(packet));
        }
    }));
    live(connect(hub, &CommHub::bytesSent, this, [this](const QByteArray &bytes, const DataSource &source) {
        if (!source.canText && receiveRoute().accepts(source))
            appendPacket(bytes, false, source);
    }));
    live(connect(hub, &CommHub::linkConnected, this,
                 [event](const QString &link, const DataSource &source) { event(tr("已连接: %1").arg(link), source); }));
    live(connect(hub, &CommHub::linkDisconnected, this,
                 [event](const QString &link, const DataSource &source) { event(tr("已断开: %1").arg(link), source); }));
    live(connect(hub, &CommHub::linkInfo, this, event));
    live(connect(hub, &CommHub::linkError, this, event));
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
    m_target = new FluentQt::CaptionLabel(this);
    m_target->setWordWrap(true);
    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(8);
    m_input = new FluentQt::LineEdit(this);
    m_input->setClearButtonEnabled(true);
    m_send = new FluentQt::PrimaryPushButton(this);
    m_send->setIcon(FluentQt::icon(FluentQt::FluentIcon::Send));
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_send);
    m_textOptions = new QWidget(this);
    auto *options = new QHBoxLayout(m_textOptions);
    options->setContentsMargins(0, 0, 0, 0);
    options->setSpacing(8);
    m_hex = new FluentQt::CheckBox(tr("HEX模式"), m_textOptions);
    m_newlineRow = new QWidget(m_textOptions);
    auto *ending = new QHBoxLayout(m_newlineRow);
    ending->setContentsMargins(0, 0, 0, 0);
    m_newline = new FluentQt::ComboBox(m_newlineRow);
    m_newline->setAccessibleName(tr("换行符"));
    addValueItems(m_newline, ManualCommand::newlines());
    m_newline->setCurrentIndex(1);
    m_newline->setFixedWidth(80);
    ending->addWidget(new FluentQt::BodyLabel(tr("换行符"), m_newlineRow));
    ending->addWidget(m_newline);
    options->addWidget(m_hex);
    m_editFields = new FluentQt::PushButton(tr("按字段发送…"), m_textOptions);
    m_editFields->setObjectName(QStringLiteral("manualSendFields"));
    options->addWidget(m_editFields);
    options->addStretch();
    options->addWidget(m_newlineRow);
    layout->addWidget(m_title);
    layout->addWidget(m_target);
    layout->addLayout(inputRow);
    layout->addWidget(m_textOptions);
    layout->addStretch();
    connect(m_hex, &QCheckBox::toggled, this, &DataSendCardWidget::refreshInputOptions);
}

void DataSendCardWidget::apply()
{
    const QString mode = text("commandMode");
    m_can = mode == ManualCommand::Can;
    m_title->setText(text("title"));
    m_target->setText(commandRoute().connectionId.isEmpty() ? tr("自动匹配唯一连接") : commandRoute().connectionId);
    m_input->setAccessibleName(m_can ? tr("CAN 报文") : tr("数据发送"));
    m_textOptions->setVisible(!m_can);
    m_hex->setChecked(mode == ManualCommand::Hex);
    if (!selectValue(m_newline, text("commandNewline")))
        m_newline->setCurrentIndex(1);
    refreshInputOptions();
    periodicSendChanged(isPeriodicSending());
}

void DataSendCardWidget::onAttached()
{
    const auto refresh = [this] {
        m_target->setText(context()->commandTargetText(m_can ? EncodedCommand::Kind::CanFrame : EncodedCommand::Kind::Bytes,
                                                     commandRoute()));
    };
    refresh();
    refreshInputOptions();
    live(connect(context()->hub(), &CommHub::linksChanged, this, refresh));
    live(connect(m_send, &QPushButton::clicked, this, &DataSendCardWidget::sendInput));
    live(connect(m_editFields, &QPushButton::clicked, this, &DataSendCardWidget::sendFields));
    live(connect(m_input, &QLineEdit::returnPressed, this, [this] {
        if (!isPeriodicSending())
            sendInput();
    }));
}

void DataSendCardWidget::sendInput()
{
    if (stopPeriodicSend())
        return;
    m_sendingFields = false;
    const QJsonObject props = inputProperties();
    const auto encode = [this, props] { return ManualCommand::encode(props, m_input->text(), frameTemplates()); };
    const int interval = CommandEncoder::periodicSendInterval(props);
    std::function<void()> onSent;
    if (interval == 0)
        onSent = [this] { m_input->selectAll(); };
    requestSend(encode(), std::move(onSent), props.value(QStringLiteral("commandMode")).toString(), interval, encode);
}

void DataSendCardWidget::sendFields()
{
    const QPointer<DataSendCardWidget> card(this);
    const auto props = inputProperties();
    const int interval = CommandEncoder::periodicSendInterval(props);
    const bool running = isPeriodicSending();
    const auto action = running ? SendFrameAction::ApplyPeriodic
                                : interval > 0 ? SendFrameAction::StartPeriodic : SendFrameAction::Send;
    const auto values = editSendFrameFields(props, frameTemplates(), m_fieldValues, visibleWindow(this), action);
    if (!values || !card || !context())
        return;
    m_fieldValues = *values;
    if (running)
        return;
    m_sendingFields = true;
    const auto encode = [this, props] {
        return CommandEncoder::frameFromFieldValues(props, m_fieldValues, frameTemplates());
    };
    requestSend(encode(), {}, ManualCommand::Hex, interval, encode);
}

void DataSendCardWidget::periodicSendChanged(bool running)
{
    const int interval = CommandEncoder::periodicSendInterval(inputProperties());
    m_send->setText(running ? (isPeriodicStarting() ? tr("取消启动") : tr("停止周期发送"))
                            : interval > 0 ? tr("开始周期发送") : tr("发送"));
    m_hex->setEnabled(!running);
    m_newlineRow->setEnabled(!running);
    m_editFields->setEnabled(!running || (m_sendingFields && !isPeriodicStarting()));
    m_editFields->setText(running && m_sendingFields ? tr("编辑发送字段…") : tr("按字段发送…"));
}

QJsonObject DataSendCardWidget::inputProperties() const
{
    QJsonObject props = properties();
    props.insert(QStringLiteral("commandMode"), m_can ? ManualCommand::Can
                                                      : m_hex->isChecked() ? ManualCommand::Hex : ManualCommand::Text);
    props.insert(QStringLiteral("commandNewline"), selectedValue(m_newline));
    return props;
}

void DataSendCardWidget::refreshInputOptions()
{
    const auto props = inputProperties();
    const bool shared = FrameTemplates::sendSettings(frameTemplates(), props).has_value();
    m_editFields->setVisible(!m_can && m_hex->isChecked() && !FrameTemplates::sendFields(frameTemplates(), props).isEmpty());
    m_newlineRow->setVisible(!m_can && !m_hex->isChecked() && !shared);
    if (m_can)
        m_input->setPlaceholderText(shared ? tr("输入 CAN ID#载荷…") : tr("CAN 链路请按 ID#数据 格式输入"));
    else
        m_input->setPlaceholderText(m_hex->isChecked() && (shared || ManualCommand::hasFraming(props))
            ? tr("输入 HEX 载荷…") : text("placeholder"));
}

void DataSendCardWidget::paintEvent(QPaintEvent *)
{
    paintCardBackground(this);
}

} // namespace fhmi
