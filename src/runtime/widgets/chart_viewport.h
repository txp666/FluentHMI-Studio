#pragma once

#include <FluentQtWidgets/Widgets/RealtimePlotWidget.h>

#include <optional>

namespace fhmi {

QColor chartSeriesColor(int index);

// Shared Fluent plot navigation and a pinned cursor for analog and digital charts.
class ChartViewport : public FluentQt::RealtimePlotWidget
{
  public:
    explicit ChartViewport(QWidget *parent);

    std::function<void(qreal)> cursorMoved;
    std::function<void()> cursorCleared;
    void hideCursor();

  protected:
    virtual QRectF plotArea() const;
    qreal toPixelX(qreal x, const QRectF &plot) const;
    std::optional<qreal> cursorPosition() const { return m_cursor; }
    void drawCursor(QPainter &painter, const QRectF &plot, const QColor &color) const;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

  private:
    bool onCursor(const QPointF &position) const;
    void placeCursor(qreal pixelX);

    std::optional<qreal> m_cursor;
    bool m_draggingCursor = false;
    bool m_clickPending = false;
    QPointF m_pressPosition;
};

} // namespace fhmi
