#pragma once

#include "comm/can/can_backend.h"
#include "runtime/widgets/link_cards.h"

#include <QtCore/QElapsedTimer>

class QTimer;

namespace FluentQt {
class EditableComboBox;
class LineEdit;
class TableView;
} // namespace FluentQt

namespace fhmi {

class CanLink;
class CanMonitorModel;
class CanMonitorProxy;

// The CAN card: adapter, channel (or a ZLG model, device and channel), bitrate and sample point, CAN FD and its
// data phase. Fields the adapter cannot set are disabled with a tooltip saying why; SocketCAN shows the ip command
// that sets its bitrate. Connecting hands the link the card's DBC file, inline signals and filter; an error in them
// refuses the connection. The header says 未连接, 已连接 (with the bus state when it is not 正常) or 连接失败; below the
// fields the card shows 收 N · 发 M while connected, or the whole reason a connection failed or dropped, wrapped and
// selectable, since the reasons (a ZLG library problem, say) are too long for the header. The adapter, channel and
// bitrate are in the card's fields and, with the rest, in the header's tooltip.
class CanConfigCardWidget : public LinkCardBase
{
  public:
    CanConfigCardWidget(const WidgetMeta &meta, QWidget *parent);
    ~CanConfigCardWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;
    Link *currentLink() const override;

  private:
    CanSettings settings() const;
    // Shows the rows and enables the fields of the selected adapter.
    void updateAdapterUi();
    // `announce`: say why there are none (the refresh button), rather than only leave the list empty.
    void refreshChannels(bool announce = false);
    void toggleConnection();
    void showConnection();
    // The card is not connected because of `reason`: the header says so, the line below the fields says why.
    void showFailure(const QString &reason);
    // The line below the fields: the counters, or (error) why the card is not connected; empty hides it.
    void showDetail(const QString &text, bool error);
    void setFieldsEnabled(bool enabled);
    // A row of widgets that sits in one field of the grid.
    QWidget *row(std::initializer_list<QPair<QWidget *, int>> widgets);

    QWidget *m_adapterRow = nullptr; // the adapter, and a ZLG model beside it
    FluentQt::ComboBox *m_adapter = nullptr;
    QWidget *m_targetRow = nullptr; // holds m_channelRow or m_zlgRow
    QWidget *m_channelRow = nullptr;
    FluentQt::EditableComboBox *m_channel = nullptr;
    FluentQt::ToolButton *m_refresh = nullptr;
    QWidget *m_zlgRow = nullptr;
    FluentQt::ComboBox *m_zlgModel = nullptr;
    FluentQt::ComboBox *m_device = nullptr;
    FluentQt::ComboBox *m_zlgChannel = nullptr;
    QWidget *m_bitrateRow = nullptr;
    FluentQt::ComboBox *m_bitrate = nullptr;
    FluentQt::ComboBox *m_samplePoint = nullptr;
    QWidget *m_fdRow = nullptr;
    FluentQt::CheckBox *m_fd = nullptr;
    FluentQt::ComboBox *m_dataBitrate = nullptr;
    FluentQt::ComboBox *m_dataSamplePoint = nullptr;
    FluentQt::CaptionLabel *m_hint = nullptr; // SocketCAN's ip command
    FluentQt::CaptionLabel *m_detail = nullptr; // below the fields: the counters, or why the card is not connected
    QPointer<QWidget> m_hintCaption; // its label in the grid, rebuilt by apply()
    QTimer *m_statusTimer = nullptr;
    CanLink *m_link = nullptr;
    QString m_shownStatus;
    QString m_lastError; // the last error toasted, so that a repeated Connect does not stack the same toast
    QElapsedTimer m_lastErrorAt;
};

// 报文监视: one row per (direction, ID) seen on the project's CAN links, updated in place; the table repaints
// about ten times a second however many frames arrive. It starts afresh when a CAN link connects while no other
// one is connected, and sorts by any column (ID ascending at first). A row above the table filters by ID (the CAN
// card's 接收过滤 syntax) and by frame type; the filter is a runtime control and is not saved with the project.
class CanMonitorWidget : public HmiWidget
{
  public:
    CanMonitorWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    // A row is as tall as the lines of its data; only rows whose line count changed are resized.
    void fitRowHeights();
    // Reads the filter row into the proxy; a filter that does not parse filters nothing and says why.
    void updateFilter();

    QWidget *m_filterRow = nullptr;
    FluentQt::LineEdit *m_idFilter = nullptr;
    FluentQt::ComboBox *m_typeFilter = nullptr;
    FluentQt::TableView *m_view = nullptr;
    CanMonitorModel *m_model = nullptr;
    CanMonitorProxy *m_sorted = nullptr;
    bool m_heightsQueued = false;
    QTimer *m_refresh = nullptr;
};

} // namespace fhmi
