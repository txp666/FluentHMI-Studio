#pragma once

#include "comm/can/can_backend.h"
#include "runtime/widgets/link_cards.h"

class QTimer;

namespace FluentQt {
class EditableComboBox;
class TableView;
} // namespace FluentQt

namespace fhmi {

class CanLink;
class CanMonitorModel;

// The CAN card: adapter, channel (or a ZLG model, device and channel), bitrate and sample point, CAN FD and its
// data phase. Fields the adapter cannot set are disabled with a tooltip saying why; SocketCAN shows the ip command
// that sets its bitrate. Connecting hands the link the card's DBC file, inline signals and filter; an error in them
// refuses the connection, with the reason on the status line and in the log. Connected, the status line reads
// <adapter> · 收 N · 发 M, and the bus state when it is not 正常.
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
    QPointer<QWidget> m_hintCaption; // its label in the grid, rebuilt by apply()
    QTimer *m_statusTimer = nullptr;
    CanLink *m_link = nullptr;
    QString m_shownStatus;
};

// 报文监视: one row per (direction, ID) seen on the project's CAN links, sorted by ID and updated in place; the
// table repaints about ten times a second however many frames arrive.
class CanMonitorWidget : public HmiWidget
{
  public:
    CanMonitorWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    FluentQt::TableView *m_view = nullptr;
    CanMonitorModel *m_model = nullptr;
    QTimer *m_refresh = nullptr;
};

} // namespace fhmi
