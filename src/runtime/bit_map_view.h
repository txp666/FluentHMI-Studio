#pragma once

#include <FluentQtWidgets/Views/ItemViews.h>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtGui/QColor>

namespace fhmi {

struct CanSignalDef;

struct BitRange
{
    int first;
    int length;
    QColor color;
};

// Maps wire bytes to B7…B0. Callers supply the occupied absolute bits.
class BitMapView : public FluentQt::TableView
{
    Q_OBJECT

  public:
    explicit BitMapView(QWidget *parent = nullptr);
    void setBits(int byteCount, const QByteArray *sample, QList<BitRange> marked = {}, int locateBit = -1);
    void setCanSignal(int byteCount, const QByteArray *sample, const CanSignalDef *signal, int locateBit = -1);

  signals:
    void bitClicked(int bit);

  protected:
    void keyPressEvent(QKeyEvent *event) override;

  private:
    class Model;
    Model *m_bits;
};

} // namespace fhmi
