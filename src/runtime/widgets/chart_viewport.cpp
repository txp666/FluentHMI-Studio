#include "runtime/widgets/chart_viewport.h"

#include "runtime/widgets/display_widgets.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <algorithm>
#include <cmath>
#include <iterator>

namespace fhmi {

QColor chartSeriesColor(int index)
{
    static const QColor light[] = {{0, 120, 212}, {244, 67, 54}, {76, 175, 80}, {255, 152, 0},
                                   {156, 39, 176}, {0, 188, 212}, {121, 85, 72}, {96, 125, 139}};
    static const QColor dark[] = {{74, 163, 223}, {255, 112, 105}, {129, 199, 132}, {255, 183, 77},
                                  {206, 147, 216}, {77, 208, 225}, {188, 170, 164}, {176, 190, 197}};
    return (isDarkTheme() ? dark : light)[index % int(std::size(light))];
}

ChartViewport::ChartViewport(QWidget *parent) : RealtimePlotWidget(parent)
{
    setMinimumSize(1, 1);
    setLegendVisible(false);
    setValueTagsVisible(false);
}

QRectF ChartViewport::plotArea() const
{
    // The same margins as FluentQt::RealtimePlotWidget, including its optional value tags.
    return QRectF(rect()).adjusted(58, 18, areValueTagsVisible() ? -58 : -18, -42);
}

qreal ChartViewport::toPixelX(qreal x, const QRectF &plot) const
{
    return plot.left() + (x - xMinimum()) / std::max(1e-6, xMaximum() - xMinimum()) * plot.width();
}

void ChartViewport::hideCursor()
{
    m_cursor.reset();
    m_draggingCursor = false;
    update();
}

void ChartViewport::drawCursor(QPainter &painter, const QRectF &plot, const QColor &color) const
{
    if (!m_cursor || *m_cursor < xMinimum() || *m_cursor > xMaximum())
        return;
    const qreal x = toPixelX(*m_cursor, plot);
    painter.setPen(QPen(color, 1, Qt::DashLine));
    painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
}

bool ChartViewport::onCursor(const QPointF &position) const
{
    const QRectF plot = plotArea();
    return m_cursor && plot.contains(position) && std::abs(position.x() - toPixelX(*m_cursor, plot)) <= 4;
}

void ChartViewport::placeCursor(qreal pixelX)
{
    const QRectF plot = plotArea();
    const qreal x = std::clamp(pixelX, plot.left(), plot.right());
    m_cursor = xMinimum() + (x - plot.left()) / std::max<qreal>(1, plot.width()) * (xMaximum() - xMinimum());
    update();
    if (cursorMoved)
        cursorMoved(*m_cursor);
}

void ChartViewport::mousePressEvent(QMouseEvent *event)
{
    const QPointF position = event->position();
    if (event->button() == Qt::LeftButton && onCursor(position)) {
        m_draggingCursor = true;
        event->accept();
        return;
    }
    RealtimePlotWidget::mousePressEvent(event);
    m_clickPending = event->button() == Qt::LeftButton && plotArea().contains(position);
    m_pressPosition = position;
}

void ChartViewport::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF position = event->position();
    if (!event->buttons().testFlag(Qt::LeftButton)) {
        m_draggingCursor = false;
        m_clickPending = false;
    }
    if (m_draggingCursor) {
        placeCursor(position.x());
        event->accept();
        return;
    }
    if (m_clickPending) {
        if ((position - m_pressPosition).manhattanLength() < QApplication::startDragDistance()) {
            event->accept();
            return;
        }
        m_clickPending = false;
    }
    if (event->buttons() == Qt::NoButton) {
        if (onCursor(position))
            setCursor(Qt::SizeHorCursor);
        else
            unsetCursor();
    }
    RealtimePlotWidget::mouseMoveEvent(event);
}

void ChartViewport::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_draggingCursor && event->button() == Qt::LeftButton) {
        m_draggingCursor = false;
        event->accept();
        return;
    }
    const bool click = m_clickPending && event->button() == Qt::LeftButton;
    m_clickPending = false;
    RealtimePlotWidget::mouseReleaseEvent(event);
    if (click)
        placeCursor(event->position().x());
}

void ChartViewport::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && plotArea().contains(event->position())) {
        m_clickPending = false;
        hideCursor();
        if (cursorCleared)
            cursorCleared();
    }
    RealtimePlotWidget::mouseDoubleClickEvent(event);
}

} // namespace fhmi
