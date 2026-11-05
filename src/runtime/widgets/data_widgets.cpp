#include "runtime/widgets/data_widgets.h"

#include "comm/can/can_link.h"
#include "core/can_frame.h"

#include "core/chart_file.h"
#include "core/hex_utils.h"
#include "core/protocol_parser.h"
#include "runtime/runtime_context.h"
#include "runtime/ui_helpers.h"
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
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtGui/QFontMetrics>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
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
// A header entry starts wide enough for its name and this, so typical readings never move the others.
const char *const kSampleReading = "-0000.00";

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

} // namespace

// The chart's plot: RealtimePlotWidget with what the Python chart's plot has besides, drawn over it: a
// loaded run as dashed curves, and a vertical cursor that a click places and a drag moves. Its own legend and
// value tags are off, as the header above it reads the values. While the user has not panned or zoomed, the
// view spans every shown curve, the dashed ones included.
class ChartPlot : public FluentQt::RealtimePlotWidget
{
  public:
    explicit ChartPlot(QWidget *parent) : FluentQt::RealtimePlotWidget(parent)
    {
        setMinimumSize(1, 1); // the chart may be smaller than the plot's own minimum
        setLegendVisible(false);
        setValueTagsVisible(false);
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
        RealtimePlotWidget::mousePressEvent(event);
        // Anywhere else in the plot a click may place the cursor.
        m_clickPending = event->button() == Qt::LeftButton && plotArea().contains(position);
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
    QRectF plotArea() const { return QRectF(rect()).adjusted(58, 18, areValueTagsVisible() ? -58 : -18, -42); }

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
    bool m_cursorOn = false;
    qreal m_cursorX = 0;
    bool m_draggingCursor = false;
    bool m_clickPending = false;
    QPointF m_pressPosition;
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
        m_displayPoints = std::clamp(ok ? typed : m_displayPoints, 10, kHistoryLimit);
        m_displayEdit->setText(QString::number(m_displayPoints));
        m_plot->setCapacity(m_displayPoints);
        refresh();
    });
    connect(save, &QToolButton::clicked, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, tr("保存完整曲线数据"), QString(),
                                                          tr("CSV文件 (*.csv);;JSON文件 (*.json)"));
        if (!path.isEmpty()) {
            saveHistory(path);
        }
    });
    connect(load, &QToolButton::clicked, this, [this]() {
        const QString path =
            QFileDialog::getOpenFileName(this, tr("加载对比曲线"), QString(), tr("CSV文件 (*.csv);;JSON文件 (*.json)"));
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
            {QStringLiteral("byteOrder"), field("byteOrder", QStringLiteral("大端(BE)"))}, // i18n: value
            {QStringLiteral("frameHeader"), field("frameHeader", QString())},
            {QStringLiteral("frameTail"), field("frameTail", QString())},
            {QStringLiteral("mbRegIndex"), field("regIndex", 0)},
            {QStringLiteral("mbDataType"), field("mbDataType", QStringLiteral("uint16"))},
            {QStringLiteral("mbRegOrder"), field("mbRegOrder", QStringLiteral("大端(AB CD)"))}, // i18n: value
            {QStringLiteral("mbFcFilter"), field("mbFcFilter", -1)},
            {QStringLiteral("mbSlaveFilter"), field("mbSlaveFilter", -1)},
            {QStringLiteral("mbArea"), field("mbArea", ModbusArea::HoldingRegisters)},
        };
        const QString name = field("name", QStringLiteral("曲线%1").arg(i + 1)).toString(); // i18n: value
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
    m_header->setTitle(text("title"));
    m_displayPoints = std::clamp(integer("displayPoints"), 10, kHistoryLimit);
    m_displayEdit->setText(QString::number(m_displayPoints));
    m_plot->setCapacity(m_displayPoints);
    m_curves = curvesFromProperties(properties());
    rebuildCurves();
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
        m_plot->addSeries(m_curves[i].name, curveColor(int(i)));
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
        m_plot->setSeriesColor(i, curveColor(i));
    }
    refresh(); // and the header's entries in the new curve colours
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
    QStringList values;
    for (size_t i = 0; i < m_curves.size(); ++i) {
        const std::optional<double> last = m_histories[i]->last();
        values.append(last ? reading(*last) : QStringLiteral("--"));
        count = std::max(count, m_histories[i]->count());
    }
    showValues(values, count - 1); // the loaded run where the live one has got to
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
        entries.append({m_curves[size_t(i)].name, live.at(i), curveColor(i), false, m_plot->isSeriesVisible(i)});
    }
    for (int i = 0; i < m_references.size(); ++i) {
        const QList<double> &values = m_references.at(i).values;
        const QString value = referenceSample >= 0 && referenceSample < values.size()
                                  ? reading(values.at(referenceSample))
                                  : QStringLiteral("--");
        entries.append({m_references.at(i).name, value, curveColor(int(m_curves.size()) + i), true,
                        m_plot->isReferenceVisible(i)});
    }
    m_header->setEntries(entries);
}

void RealtimeChartWidget::toggleCurve(int index)
{
    if (index < m_plot->seriesCount()) {
        m_plot->setSeriesVisible(index, !m_plot->isSeriesVisible(index)); // and the plot fits the shown curves
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
    return text("recordMode") == QStringLiteral("逐行记录"); // i18n: value
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
        m_columns = {QStringLiteral("字段"), QStringLiteral("值")}; // i18n: value
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
        const QString timeColumn = QStringLiteral("时间"); // i18n: value
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

void ModbusSlaveTableWidget::showDesign(const QJsonObject *cardProperties)
{
    if (cardProperties) {
        m_design = ModbusSlaveStore::fromProperties(*cardProperties);
    } else {
        m_design.reset();
    }
    rebuild();
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
    rebuild();
}

void ModbusSlaveTableWidget::bindSlave()
{
    const QList<ModbusSlave *> slaves = context()->hub()->modbusSlaves();
    ModbusSlave *first = slaves.isEmpty() ? nullptr : slaves.first();
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
        auto *hint = new QTableWidgetItem(tr("没有从站链路卡片"));
        hint->setFlags(Qt::ItemIsEnabled);
        m_table->setItem(0, 0, hint);
        m_table->setSpan(0, 0, 1, 2);
        m_filling = false;
        return;
    }
    // Values are edited only while running, in the slave's own table.
    m_table->setEditTriggers(isLive() ? QAbstractItemView::DoubleClicked : QAbstractItemView::NoEditTriggers);
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
    if (m_filling || !m_slave || item->column() != 1) {
        return;
    }
    const QString t = table();
    const int address = m_slave->store().area(t).start + item->row();
    const std::optional<quint16> value = parse(item->text());
    if (!value || !m_slave->write(t, address, {*value})) {
        m_filling = true;
        item->setText(display(m_slave->store().value(t, address))); // not a value: the old one stays
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
    live(connect(hub, &CommHub::linkConnected, this,
                 [this](const QString &link) { appendLog(tr("已连接: %1").arg(link), QStringLiteral("ok")); }));
    live(connect(hub, &CommHub::linkDisconnected, this,
                 [this](const QString &link) { appendLog(tr("已断开: %1").arg(link), QStringLiteral("warn")); }));
    live(connect(hub, &CommHub::linkError, this,
                 [this](const QString &message) { appendLog(tr("错误: %1").arg(message), QStringLiteral("error")); }));
    live(connect(hub, &CommHub::linkWarning, this,
                 [this](const QString &message) { appendLog(tr("警告: %1").arg(message), QStringLiteral("warn")); }));
    live(connect(hub, &CommHub::linkInfo, this, [this](const QString &message) { appendLog(message); }));
    live(connect(hub, &CommHub::messageReceived, this, [this](const Message &message) {
        // A slave's own change is no frame received: its requests and replies are logged instead.
        if (context()->isActive() && !context()->hub()->isSlaveData()) {
            appendLog(messageText(message));
        }
    }));
    live(connect(hub, &CommHub::slaveLog, this, &LogBoxWidget::appendLog));
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
    // A CAN link's frame line is text that already shows its bytes in HEX.
    if (context()->hub()->carriesCanText()) {
        return QString::fromUtf8(bytes);
    }
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
    CommHub *hub = context()->hub();
    if (!hub->isConnected()) {
        flashTip(tr("未连接"));
        return;
    }
    // CAN links take cansend's ID#DATA; the other links get the text as always.
    const bool can = !hub->connectedCanLinks().isEmpty();
    std::optional<CanFrame> frame;
    if (can) {
        frame = Can::parseSendText(input);
        if (!frame) {
            flashTip(tr("CAN 链路请按 ID#数据 格式输入"));
            return;
        }
    }
    bool sent = false;
    if (hub->hasConnectedByteLink()) {
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
        sent = context()->send(CommandResult::ok(command));
    }
    if (frame) {
        QString error;
        if (hub->sendCanFrame(*frame, &error) > 0) {
            sent = true;
        } else if (!error.isEmpty()) {
            context()->notify(tr("提示"), error, NotifyType::Error);
        }
    }
    if (sent) {
        m_input->selectAll();
    }
}

void DataSendCardWidget::paintEvent(QPaintEvent *)
{
    paintCardBackground(this);
}

} // namespace fhmi
