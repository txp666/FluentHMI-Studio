#pragma once

#include "core/hex_utils.h"

#include <QtCore/QList>
#include <QtGui/QColor>
#include <QtWidgets/QAbstractScrollArea>

class QBoxLayout;
class QLabel;

namespace FluentQt {
class BodyLabel;
}

namespace fhmi {

class BitMapView;

enum class FrameByteKind { Data, Header, Length, Fixed, Field, Checksum, Footer, Padding };

struct FrameByteSpan
{
    QString key;
    QString label;
    qsizetype offset = 0;
    qsizetype size = 0;
    FrameByteKind kind = FrameByteKind::Data;
    int tint = 0;
};

QColor frameByteColor(FrameByteKind kind, int tint = 0);
QLabel *frameByteMarker(QWidget *parent, FrameByteKind kind, int tint = 0);
void setFrameByteMarker(QLabel *marker, FrameByteKind kind, int tint = 0);
QList<FrameByteSpan> frameEnvelopeSpans(const HexFrameLayout &layout, qsizetype size);

class FrameCoveragePreview : public QWidget
{
  public:
    explicit FrameCoveragePreview(QWidget *parent);
    void setFrame(const QByteArray &frame, const HexFrameLayout &layout, const QByteArray &calculated = {});

  private:
    FluentQt::BodyLabel *m_lengthCoverage;
    FluentQt::BodyLabel *m_checksumCoverage;
    FluentQt::BodyLabel *m_checksumValues;
};

class FrameBytesView : public QAbstractScrollArea
{
    Q_OBJECT

  public:
    explicit FrameBytesView(QWidget *parent = nullptr);
    const QByteArray &bytes() const { return m_bytes; }
    void setFrame(QByteArray bytes, QList<FrameByteSpan> spans, const HexFrameLayout &layout,
                  qsizetype applicationSize = -1);
    void selectPart(const QString &key);
    void markRange(qsizetype offset, qsizetype size);
    void locateMarkedRange();
    FrameByteSpan highlightedSpan() const;
    void activateByte(int offset);

  signals:
    void partClicked(const QString &key);
    void byteClicked(int offset);
    void diagramChanged();

  protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

  private:
    int selectedSpan(qsizetype offset) const;
    int displayedSpan(qsizetype offset) const;
    void scrollToRange(qsizetype offset, qsizetype size);
    void updateScrollRange();

    QByteArray m_bytes;
    QList<FrameByteSpan> m_spans;
    QList<int> m_spanAt;
    QString m_selected;
    qsizetype m_markedOffset = 0;
    qsizetype m_markedSize = 0;
    std::optional<FrameByteRange> m_lengthCoverage;
    std::optional<FrameByteRange> m_checksumCoverage;
};

// Both diagrams of a binary frame share the sample and the active field.
class FrameDiagram : public QWidget
{
  public:
    explicit FrameDiagram(QWidget *parent = nullptr, QBoxLayout *modeRow = nullptr);
    FrameBytesView *byteView() const { return m_bytes; }

  private:
    void updateBits();
    FrameBytesView *m_bytes;
    BitMapView *m_bits;
    FrameByteRange m_bitSelection{};
};

} // namespace fhmi
