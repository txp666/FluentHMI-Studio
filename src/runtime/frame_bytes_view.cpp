#include "runtime/frame_bytes_view.h"

#include "core/i18n.h"
#include "core/protocol_parser.h"
#include "runtime/bit_map_view.h"

#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>

#include <QtCore/QCoreApplication>
#include <QtGui/QFontDatabase>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>

namespace fhmi {
namespace {

constexpr int CellWidth = 60;
constexpr int BandHeight = 22;
constexpr int CellHeight = 40;
constexpr int CoverageTrackSpacing = 7;

bool darkTheme()
{
    return FluentQt::ThemeManager::instance()->effectiveTheme() == FluentQt::Theme::Dark;
}

QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }

enum class CoverageKind { Length, Checksum };

QPen coveragePen(CoverageKind kind)
{
    if (kind == CoverageKind::Checksum)
        return QPen(QColor(darkTheme() ? 0xF1A376 : 0xBB571F), 3, Qt::SolidLine, Qt::RoundCap);
    QPen pen(QColor(darkTheme() ? 0x86B9F6 : 0x2869B2), 3, Qt::SolidLine, Qt::FlatCap);
    pen.setDashPattern({2, 1.5});
    return pen;
}

QString byteRangeText(qsizetype offset, qsizetype size)
{
    return size == 1 ? tr("整帧字节 %1").arg(offset)
                     : tr("整帧字节 %1–%2").arg(offset).arg(offset + size - 1);
}

class CoverageMarker : public QWidget
{
  public:
    CoverageMarker(CoverageKind kind, QWidget *parent) : QWidget(parent), m_kind(kind)
    {
        setFixedSize(20, 5);
        connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged,
                this, qOverload<>(&QWidget::update));
    }

  private:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(coveragePen(m_kind));
        painter.drawLine(2, height() / 2, width() - 2, height() / 2);
    }

    CoverageKind m_kind;
};

FluentQt::BodyLabel *addCoverageLegend(QVBoxLayout *layout, CoverageKind kind, const QString &name)
{
    auto *row = new QWidget(layout->parentWidget());
    auto *horizontal = new QHBoxLayout(row);
    horizontal->setContentsMargins(0, 0, 0, 0);
    horizontal->setSpacing(6);
    horizontal->addWidget(new CoverageMarker(kind, row));
    auto *caption = new FluentQt::BodyLabel(row);
    caption->setObjectName(name);
    caption->setWordWrap(true);
    horizontal->addWidget(caption, 1);
    layout->addWidget(row);
    return caption;
}

QString coverageText(const QByteArray &frame, const FrameByteRange &range, const QString &format,
                     const QString &empty)
{
    if (range.size == 0)
        return empty;
    constexpr qsizetype previewLimit = 12;
    auto hex = toHexString(frame.mid(range.offset, std::min(range.size, previewLimit)));
    if (range.size > previewLimit)
        hex += QStringLiteral(" …");
    return format.arg(byteRangeText(range.offset, range.size), hex);
}

} // namespace

QColor frameByteColor(FrameByteKind kind, int tint)
{
    static constexpr std::array light{0xE8ECEF, 0xD7EFFA, 0xF9E9A9, 0xEDE1F8,
                                      0xD6EEDD, 0xFBE1D3, 0xE0E7FA, 0xEBEBEB};
    static constexpr std::array dark{0x393E44, 0x244956, 0x544825, 0x4B3A5D,
                                     0x2C493D, 0x593D33, 0x384463, 0x414144};
    static constexpr std::array fieldsLight{0xD6EEDD, 0xCFEFE9, 0xE7ECCA, 0xF4DFEA, 0xD8E8F8, 0xE7E1FA};
    static constexpr std::array fieldsDark{0x2C493D, 0x214B49, 0x454B31, 0x573948, 0x2F465E, 0x463D5D};
    if (kind == FrameByteKind::Field)
        return QColor::fromRgb((darkTheme() ? fieldsDark : fieldsLight).at(tint % fieldsLight.size()));
    return QColor::fromRgb((darkTheme() ? dark : light).at(int(kind)));
}

void setFrameByteMarker(QLabel *marker, FrameByteKind kind, int tint)
{
    marker->setProperty("byteKind", int(kind));
    marker->setProperty("byteTint", tint);
    marker->setStyleSheet(QStringLiteral("background: %1; border-radius: 2px;").arg(frameByteColor(kind, tint).name()));
}

QLabel *frameByteMarker(QWidget *parent, FrameByteKind kind, int tint)
{
    auto *marker = new QLabel(parent);
    marker->setFixedSize(10, 10);
    setFrameByteMarker(marker, kind, tint);
    QObject::connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, marker,
                     [marker] {
                         setFrameByteMarker(marker, FrameByteKind(marker->property("byteKind").toInt()),
                                            marker->property("byteTint").toInt());
                     });
    return marker;
}

QList<FrameByteSpan> frameEnvelopeSpans(const HexFrameLayout &layout, qsizetype size)
{
    const qsizetype footer = size - layout.footerSize;
    return {
        {QStringLiteral("header"), tr("帧头"), 0, layout.headerSize, FrameByteKind::Header},
        {QStringLiteral("length"), tr("长度"), layout.lengthOffset, layout.lengthSize, FrameByteKind::Length},
        {QStringLiteral("checksum"), tr("校验"), layout.checksumOffset(size), layout.checksumSize, FrameByteKind::Checksum},
        {QStringLiteral("footer"), tr("帧尾"), footer, layout.footerSize, FrameByteKind::Footer},
    };
}

FrameCoveragePreview::FrameCoveragePreview(QWidget *parent) : QWidget(parent)
{
    auto *vertical = new QVBoxLayout(this);
    vertical->setContentsMargins(0, 0, 0, 0);
    vertical->setSpacing(4);
    m_lengthCoverage = addCoverageLegend(vertical, CoverageKind::Length, QStringLiteral("frameLengthCoverage"));
    m_checksumCoverage = addCoverageLegend(vertical, CoverageKind::Checksum, QStringLiteral("frameChecksumCoverage"));
    m_checksumValues = new FluentQt::BodyLabel(this);
    m_checksumValues->setObjectName(QStringLiteral("frameChecksumValues"));
    m_checksumValues->setWordWrap(true);
    vertical->addWidget(m_checksumValues);
    hide();
}

void FrameCoveragePreview::setFrame(const QByteArray &frame, const HexFrameLayout &layout, const QByteArray &calculated)
{
    const auto length = layout.lengthCoverage(frame.size());
    const auto checksum = layout.checksumCoverage(frame.size());
    m_lengthCoverage->parentWidget()->setVisible(length.has_value());
    m_checksumCoverage->parentWidget()->setVisible(checksum.has_value());
    m_checksumValues->setVisible(checksum.has_value());
    if (length)
        m_lengthCoverage->setText(coverageText(frame, *length, fhmi::tr("长度覆盖 · %1 · %2"), fhmi::tr("长度覆盖 · 0 字节")));
    setVisible(length || checksum);
    if (!checksum)
        return;
    m_checksumCoverage->setText(coverageText(frame, *checksum, fhmi::tr("参与校验 · %1 · %2"), fhmi::tr("参与校验 · 0 字节")));
    const auto at = layout.checksumOffset(frame.size());
    const auto actual = frame.mid(at, layout.checksumSize);
    const auto onWire = toHexString(actual);
    const bool matched = actual == calculated;
    const auto result = calculated.isEmpty()
        ? fhmi::tr("写入校验值 %1 · %2").arg(onWire, byteRangeText(at, layout.checksumSize))
        : fhmi::tr("报文值 %1 · 计算值 %2 · %3").arg(onWire, toHexString(calculated),
              matched ? fhmi::tr("校验通过") : fhmi::tr("校验值不匹配"));
    m_checksumValues->setText(translated(HexOptions::checksums().at(int(layout.checksum))) + QStringLiteral(" · ") + result);
    const bool mismatch = !calculated.isEmpty() && !matched;
    m_checksumValues->setTextColor(mismatch ? QColor(0xC42B1C) : QColor(0x16663B),
                                   mismatch ? QColor(0xFF99A4) : QColor(0x72D499));
}

FrameBytesView::FrameBytesView(QWidget *parent) : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("frameBytesView"));
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    horizontalScrollBar()->setSingleStep(CellWidth);
    auto *bar = new FluentQt::ScrollBar(Qt::Horizontal, this);
    bar->attachToScrollArea(this, Qt::Horizontal);
    viewport()->setAutoFillBackground(false);
    setFixedHeight(BandHeight + CellHeight + 16);
    setAccessibleName(fhmi::tr("字节布局"));
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(), qOverload<>(&QWidget::update));
    auto *theme = FluentQt::ThemeManager::instance();
    connect(theme, &FluentQt::ThemeManager::effectiveThemeChanged, viewport(), qOverload<>(&QWidget::update));
    connect(theme, &FluentQt::ThemeManager::accentColorChanged, viewport(), qOverload<>(&QWidget::update));
}

void FrameBytesView::setFrame(QByteArray bytes, QList<FrameByteSpan> spans, const HexFrameLayout &layout,
                             qsizetype applicationSize)
{
    m_bytes = std::move(bytes);
    m_spans = std::move(spans);
    const auto frameSize = applicationSize < 0 ? m_bytes.size() : applicationSize;
    m_lengthCoverage = layout.lengthCoverage(frameSize);
    m_checksumCoverage = layout.checksumCoverage(frameSize);
    const bool twoTracks = m_lengthCoverage && m_lengthCoverage->size > 0 && m_checksumCoverage && m_checksumCoverage->size > 0;
    setFixedHeight(BandHeight + CellHeight + 16 + (twoTracks ? CoverageTrackSpacing : 0));
    m_spanAt.fill(-1, m_bytes.size());
    for (int i = 0; i < m_spans.size(); ++i) {
        const auto &span = m_spans.at(i);
        const qsizetype end = std::min(span.offset + span.size, m_bytes.size());
        for (qsizetype at = std::max<qsizetype>(0, span.offset); at < end; ++at) {
            if (m_spanAt.at(at) < 0)
                m_spanAt[at] = i;
        }
    }
    setAccessibleDescription(toHexString(m_bytes));
    updateScrollRange();
    viewport()->update();
    emit diagramChanged();
}

void FrameBytesView::selectPart(const QString &key)
{
    m_selected = key;
    for (const auto &span : std::as_const(m_spans)) {
        if (span.key != key || span.size == 0)
            continue;
        scrollToRange(span.offset, span.size);
        break;
    }
    viewport()->update();
    emit diagramChanged();
}

void FrameBytesView::markRange(qsizetype offset, qsizetype size)
{
    m_markedOffset = offset;
    m_markedSize = size;
    viewport()->update();
    emit diagramChanged();
}

FrameByteSpan FrameBytesView::highlightedSpan() const
{
    for (const auto &span : m_spans)
        if (!m_selected.isEmpty() && span.key == m_selected && span.size > 0)
            return span;
    return {{}, {}, m_markedOffset, m_markedSize, FrameByteKind::Field, 1};
}

void FrameBytesView::locateMarkedRange()
{
    if (m_markedSize > 0)
        scrollToRange(m_markedOffset, m_markedSize);
}

void FrameBytesView::scrollToRange(qsizetype offset, qsizetype size)
{
    const int start = int(offset * CellWidth);
    const int end = start + int(size * CellWidth);
    if (start < horizontalScrollBar()->value() || end > horizontalScrollBar()->value() + viewport()->width())
        horizontalScrollBar()->setValue(start);
}

int FrameBytesView::selectedSpan(qsizetype offset) const
{
    if (m_selected.isEmpty())
        return -1;
    for (int i = 0; i < m_spans.size(); ++i) {
        const auto &span = m_spans.at(i);
        if (span.key == m_selected && span.offset <= offset && offset < span.offset + span.size)
            return i;
    }
    return -1;
}

int FrameBytesView::displayedSpan(qsizetype offset) const
{
    const int span = m_spanAt.at(offset);
    if (span < 0 || m_spans.at(span).kind == FrameByteKind::Field || m_spans.at(span).kind == FrameByteKind::Data) {
        if (const int selected = selectedSpan(offset); selected >= 0)
            return selected;
    }
    return span;
}

void FrameBytesView::updateScrollRange()
{
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setRange(0, int(std::max<qsizetype>(0, m_bytes.size() * CellWidth - viewport()->width())));
}

void FrameBytesView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRange();
}

void FrameBytesView::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor ink = darkTheme() ? QColor(0xF3F3F3) : QColor(0x172B35);
    const QColor outline = FluentQt::ThemeManager::instance()->accentColor();
    QFont offsetFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    offsetFont.setPixelSize(11);
    QFont byteFont = offsetFont;
    byteFont.setPixelSize(15);
    byteFont.setBold(true);
    const int scroll = horizontalScrollBar()->value();
    const qsizetype first = scroll / CellWidth;
    const qsizetype end = std::min<qsizetype>(m_bytes.size(), (scroll + viewport()->width() + CellWidth - 1) / CellWidth);
    const auto background = [this](int i) {
        return i < 0 ? frameByteColor(FrameByteKind::Data) : frameByteColor(m_spans.at(i).kind, m_spans.at(i).tint);
    };
    const auto tile = [&painter](const QRect &rect, QColor fill, QColor border = {}) {
        painter.setPen(border.isValid() ? QPen(border, 2) : QPen(Qt::NoPen));
        painter.setBrush(fill);
        painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 4, 4);
    };

    QFont bandFont = font();
    bandFont.setPixelSize(12);
    painter.setFont(bandFont);
    for (qsizetype start = first; start < end;) {
        const int span = displayedSpan(start);
        qsizetype next = start + 1;
        while (next < end && displayedSpan(next) == span)
            ++next;
        const int x = int(start * CellWidth) - scroll;
        const QRect band(x + 1, 0, int(next - start) * CellWidth - 2, BandHeight);
        tile(band, background(span));
        const QRect visible = band.intersected(viewport()->rect()).adjusted(4, 0, -4, 0);
        const QString label = span < 0 ? fhmi::tr("数据") : m_spans.at(span).label;
        painter.setPen(ink);
        painter.drawText(visible, Qt::AlignCenter, painter.fontMetrics().elidedText(label, Qt::ElideRight, visible.width()));
        start = next;
    }
    for (qsizetype at = first; at < end; ++at) {
        const QRect cell(int(at * CellWidth) - scroll + 1, BandHeight + 1, CellWidth - 2, CellHeight - 1);
        tile(cell, background(displayedSpan(at)), selectedSpan(at) >= 0 ? outline : QColor());
        painter.setPen(ink);
        painter.setFont(offsetFont);
        painter.drawText(cell.adjusted(0, 3, 0, -19), Qt::AlignCenter, QString::number(at));
        painter.setFont(byteFont);
        painter.drawText(cell.adjusted(0, 15, 0, -2), Qt::AlignCenter,
                         QString::number(quint8(m_bytes.at(at)), 16).rightJustified(2, u'0').toUpper());
        if (at >= m_markedOffset && at - m_markedOffset < m_markedSize) {
            painter.setPen(QPen(outline, 3, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(cell.bottomLeft() + QPoint(7, -2), cell.bottomRight() + QPoint(-7, -2));
        }
    }
    int y = BandHeight + CellHeight + 5;
    const auto drawCoverage = [&](const std::optional<FrameByteRange> &range, CoverageKind kind) {
        if (!range || range->size == 0)
            return;
        const auto coveredFirst = std::max(first, range->offset);
        const auto coveredEnd = std::min(end, range->offset + range->size);
        if (coveredEnd > coveredFirst) {
            painter.setPen(coveragePen(kind));
            painter.drawLine(int(coveredFirst * CellWidth) - scroll + 3, y, int(coveredEnd * CellWidth) - scroll - 3, y);
        }
        y += CoverageTrackSpacing;
    };
    drawCoverage(m_lengthCoverage, CoverageKind::Length);
    drawCoverage(m_checksumCoverage, CoverageKind::Checksum);
}

void FrameBytesView::activateByte(int offset)
{
    emit byteClicked(offset);
    const int span = displayedSpan(offset);
    if (span >= 0 && !m_spans.at(span).key.isEmpty()) {
        const QString key = m_spans.at(span).key;
        selectPart(key);
        emit partClicked(key);
    }
}

void FrameBytesView::mousePressEvent(QMouseEvent *event)
{
    const int offset = (horizontalScrollBar()->value() + int(event->position().x())) / CellWidth;
    if (event->button() == Qt::LeftButton && event->position().y() < BandHeight + CellHeight &&
        offset >= 0 && offset < m_bytes.size()) {
        activateByte(offset);
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void FrameBytesView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        event->accept();
    else
        QAbstractScrollArea::mouseReleaseEvent(event);
}

void FrameBytesView::wheelEvent(QWheelEvent *event)
{
    const QPoint delta = event->pixelDelta().isNull() ? event->angleDelta() : event->pixelDelta();
    if (!horizontalScrollBar()->maximum()) {
        event->ignore();
        return;
    }
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - (delta.x() ? delta.x() : delta.y()));
    event->accept();
}

FrameDiagram::FrameDiagram(QWidget *parent, QBoxLayout *modeRow)
    : QWidget(parent), m_bytes(new FrameBytesView(this)), m_bits(new BitMapView(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto *mode = new FluentQt::SegmentedWidget(this);
    mode->setObjectName(QStringLiteral("frameDiagramMode"));
    mode->addItem(QStringLiteral("bytes"), fhmi::tr("字节"));
    mode->addItem(QStringLiteral("bits"), fhmi::tr("位图"));
    mode->setCurrentItem(QStringLiteral("bytes"));
    (modeRow ? modeRow : layout)->addWidget(mode, 0, Qt::AlignLeft);
    layout->addWidget(m_bytes);
    m_bits->setObjectName(QStringLiteral("hexFrameBits"));
    m_bits->verticalHeader()->setDefaultSectionSize(20);
    m_bits->hide();
    layout->addWidget(m_bits);
    connect(mode, &FluentQt::SegmentedWidget::currentItemChanged, this, [this](const QString &key) {
        const bool showBits = key == QLatin1String("bits");
        m_bytes->setVisible(!showBits);
        m_bits->setVisible(showBits);
        m_bitSelection = {};
        updateBits();
    });
    connect(m_bytes, &FrameBytesView::diagramChanged, this, &FrameDiagram::updateBits);
    connect(m_bits, &BitMapView::bitClicked, this, [this](int bit) { m_bytes->activateByte(bit / 8); });
    connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged,
            this, &FrameDiagram::updateBits);
}

void FrameDiagram::updateBits()
{
    if (m_bits->isHidden())
        return;
    const auto &bytes = m_bytes->bytes();
    const auto span = m_bytes->highlightedSpan();
    QList<BitRange> marked;
    if (span.size > 0)
        marked.append({int(span.offset * 8), int(span.size * 8), frameByteColor(span.kind, span.tint)});
    const bool locate = span.size > 0 && (span.offset != m_bitSelection.offset || span.size != m_bitSelection.size);
    m_bits->setBits(int(bytes.size()), &bytes, std::move(marked), locate ? int(span.offset * 8) : -1);
    const int visibleRows = int(std::clamp<qsizetype>(bytes.size(), 1, 9));
    m_bits->setFixedHeight(m_bits->horizontalHeader()->sizeHint().height() + 2 * m_bits->frameWidth() +
                          visibleRows * m_bits->verticalHeader()->defaultSectionSize());
    m_bitSelection = {span.offset, span.size};
}

} // namespace fhmi
