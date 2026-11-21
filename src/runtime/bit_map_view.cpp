#include "runtime/bit_map_view.h"

#include "core/can_signal.h"
#include "runtime/frame_bytes_view.h"

#include <FluentQtWidgets/StyleSheet.h>
#include <QtCore/QAbstractTableModel>
#include <QtGui/QBrush>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QHeaderView>

#include <algorithm>

namespace fhmi {
namespace {

int absoluteBit(const QModelIndex &cell) { return cell.row() * 8 + 7 - cell.column(); }

} // namespace

class BitMapView::Model final : public QAbstractTableModel
{
  public:
    using QAbstractTableModel::QAbstractTableModel;

    void setBits(int rows, const QByteArray *sample, QList<BitRange> marked)
    {
        const bool sizeChanged = m_rows != rows;
        if (sizeChanged)
            beginResetModel();
        m_rows = rows;
        m_sample = sample ? *sample : QByteArray();
        m_hasSample = sample != nullptr;
        m_marked = std::move(marked);
        if (sizeChanged) {
            endResetModel();
        } else if (m_rows > 0) {
            emit headerDataChanged(Qt::Vertical, 0, m_rows - 1);
            repaintBits();
        }
    }

    void repaintBits()
    {
        if (m_rows > 0)
            emit dataChanged(index(0, 0), index(m_rows - 1, 7));
    }

    int rowCount(const QModelIndex &parent) const override { return parent.isValid() ? 0 : m_rows; }
    int columnCount(const QModelIndex &parent) const override { return parent.isValid() ? 0 : 8; }

    QVariant headerData(int section, Qt::Orientation direction, int role) const override
    {
        if (role != Qt::DisplayRole)
            return {};
        if (direction == Qt::Horizontal)
            return QStringLiteral("B%1").arg(7 - section);
        if (section < m_sample.size()) {
            const QString hex = QStringLiteral("%1").arg(quint8(m_sample.at(section)), 2, 16, QLatin1Char('0')).toUpper();
            return tr("字节 %1 · %2").arg(section).arg(hex);
        }
        return tr("字节 %1").arg(section);
    }

    QVariant data(const QModelIndex &cell, int role) const override
    {
        if (!cell.isValid())
            return {};
        const int bit = absoluteBit(cell);
        if (role == Qt::TextAlignmentRole)
            return int(Qt::AlignCenter);
        if (role == Qt::BackgroundRole) {
            const auto marked = std::find_if(m_marked.cbegin(), m_marked.cend(), [bit](const BitRange &range) {
                return bit >= range.first && bit - range.first < range.length;
            });
            return marked != m_marked.cend() ? QVariant(QBrush(marked->color)) : QVariant();
        }
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole || role == Qt::ToolTipRole) {
            const QString value = cell.row() < m_sample.size()
                ? QString::number((quint8(m_sample.at(cell.row())) >> (7 - cell.column())) & 1)
                : m_hasSample ? QStringLiteral("—") : QString::number(bit);
            return role == Qt::DisplayRole ? value : tr("第 %1 位 · %2").arg(bit).arg(value);
        }
        return {};
    }

  private:
    int m_rows = 0;
    bool m_hasSample = false;
    QByteArray m_sample;
    QList<BitRange> m_marked;
};

BitMapView::BitMapView(QWidget *parent) : FluentQt::TableView(parent), m_bits(new Model(this))
{
    setModel(m_bits);
    setAccessibleName(tr("报文位图"));
    setEditTriggers(NoEditTriggers);
    setSelectionMode(NoSelection);
    setShowGrid(false);
    setBorderVisible(true);
    setAlternatingRowColors(true);
    setWordWrap(false);
    setTextElideMode(Qt::ElideNone);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    const auto compactCells = QStringLiteral("QTableView::item { padding-left: 2px; padding-right: 2px; }");
    FluentQt::FluentStyleSheet::setCustomStyleSheet(this, compactCells, compactCells);
    horizontalHeader()->setMinimumSectionSize(32);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    verticalHeader()->setDefaultSectionSize(24);
    verticalHeader()->setSectionsClickable(false);
    verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    connect(this, &QTableView::clicked, this, [this](const QModelIndex &cell) { emit bitClicked(absoluteBit(cell)); });
}

void BitMapView::keyPressEvent(QKeyEvent *event)
{
    const auto current = currentIndex();
    if (current.isValid() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
                              event->key() == Qt::Key_Space)) {
        emit bitClicked(absoluteBit(current));
        event->accept();
        return;
    }
    FluentQt::TableView::keyPressEvent(event);
}

void BitMapView::setBits(int byteCount, const QByteArray *sample, QList<BitRange> marked, int locateBit)
{
    m_bits->setBits(byteCount, sample, std::move(marked));
    if (locateBit >= 0 && locateBit / 8 < byteCount)
        scrollTo(m_bits->index(locateBit / 8, 7 - locateBit % 8));
}

void BitMapView::setCanSignal(int byteCount, const QByteArray *sample, const CanSignalDef *signal, int locateBit)
{
    QList<BitRange> marked;
    if (signal) {
        const auto color = frameByteColor(FrameByteKind::Field, 1);
        for (const int bit : signal->bitPositions())
            marked.append({bit, 1, color});
    }
    setBits(byteCount, sample, std::move(marked), locateBit);
}

} // namespace fhmi
