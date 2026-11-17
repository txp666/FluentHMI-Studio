#pragma once

#include "comm/can/can_backend.h"
#include "runtime/widgets/link_cards.h"

class QFileDialog;

class QTimer;

namespace FluentQt {
class EditableComboBox;
class LineEdit;
class SpinBox;
class TableView;
class PushButton;
} // namespace FluentQt

namespace fhmi {

class CanLink;
class CanMonitorModel;
class CanMonitorProxy;
class CanRecorder;

// The CAN card: adapter, channel (or a ZLG model, device and channel), bitrate and sample point, CAN FD and its
// data phase. Fields the adapter cannot set are disabled with a tooltip saying why; SocketCAN shows the ip command
// that sets its bitrate. Connecting hands the link the card's DBC file, inline signals and filter; an error in them
// refuses the connection. The header says 未连接, 已连接 (with the bus state when it is not 正常) or 连接失败; below the
// fields the card shows 收 N · 发 M while connected. Failure details appear in a notification and remain in the
// status tooltip, without changing the card's height. The adapter, channel and
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
    void resetSamplePointOptions();
    // Shows the rows and enables the fields of the selected adapter.
    void updateAdapterUi();
    // `announce`: say why there are none (the refresh button), rather than only leave the list empty.
    void refreshChannels(bool announce = false);
    void toggleConnection();
    void showTimingCalculator();
    void showConnection();
    // The card is not connected because of `reason`: keep its brief status and full tooltip.
    void showFailure(const QString &reason);
    // The line below the fields holds only counters; empty hides it.
    void showCounters(const QString &text);
    void setFieldsEnabled(bool enabled);
    // A row of widgets that sits in one field of the grid.
    QWidget *row(std::initializer_list<QPair<QWidget *, int>> widgets);

    QWidget *m_adapterRow = nullptr; // the adapter, and a ZLG model beside it
    FluentQt::ComboBox *m_adapter = nullptr;
    QWidget *m_targetRow = nullptr; // holds m_channelRow or m_zlgRow
    QWidget *m_channelRow = nullptr;
    FluentQt::EditableComboBox *m_channel = nullptr;
    FluentQt::ToolButton *m_refresh = nullptr;
    FluentQt::ToolButton *m_detect = nullptr;
    int m_scanGeneration = 0;
    bool m_scanning = false;
    QWidget *m_zlgRow = nullptr;
    FluentQt::ComboBox *m_zlgModel = nullptr;
    FluentQt::ComboBox *m_device = nullptr;
    FluentQt::ComboBox *m_zlgChannel = nullptr;
    QWidget *m_bitrateRow = nullptr;
    QPointer<QWidget> m_bitrateCaption;
    FluentQt::ComboBox *m_bitrate = nullptr;
    FluentQt::ComboBox *m_samplePoint = nullptr;
    FluentQt::CaptionLabel *m_sampleCaption = nullptr;
    QWidget *m_fdRow = nullptr;
    QPointer<QWidget> m_fdCaption;
    FluentQt::CheckBox *m_fd = nullptr;
    FluentQt::ComboBox *m_dataBitrate = nullptr;
    FluentQt::ComboBox *m_dataSamplePoint = nullptr;
    FluentQt::CaptionLabel *m_dataSampleCaption = nullptr;
    FluentQt::LineEdit *m_customTiming = nullptr;
    QWidget *m_customRow = nullptr;
    FluentQt::PushButton *m_calculate = nullptr;
    QPointer<QWidget> m_customTimingCaption;
    FluentQt::CaptionLabel *m_hint = nullptr;   // SocketCAN's ip command, or ZLG's effective bit timing
    FluentQt::CaptionLabel *m_counters = nullptr;
    QPointer<QWidget> m_hintCaption; // its label in the grid, rebuilt by apply()
    QTimer *m_statusTimer = nullptr;
    CanLink *m_link = nullptr;
    QString m_shownStatus;
    QString m_connectionError;
};

// CAN summary or bounded chronological log. Both modes filter by ID/type and export the retained visible rows.
// Frames are delivered in batches to the table at most ten times a second.
class CanMonitorWidget : public HmiWidget
{
  public:
    CanMonitorWidget(const WidgetMeta &meta, QWidget *parent);
    ~CanMonitorWidget() override;
    bool saveCsv(const QString &path, QString *error = nullptr);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    // A row is as tall as the lines of its data; only rows whose line count changed are resized.
    void fitRowHeights();
    // Reads the filter row into the proxy; a filter that does not parse filters nothing and says why.
    void updateFilter();
    void updateDisplayMode();
    void chooseExportFile();
    QString recordingDirectory() const;
    void chooseRecordingDirectory(bool change = false);
    void showRecordingDirectoryMenu();
    void updateRecordingControls();
    void updateRecording();

    QWidget *m_filterRow = nullptr;
    FluentQt::LineEdit *m_idFilter = nullptr;
    FluentQt::ComboBox *m_typeFilter = nullptr;
    FluentQt::ComboBox *m_displayMode = nullptr;
    FluentQt::SpinBox *m_maxMessages = nullptr;
    FluentQt::CheckBox *m_autoScroll = nullptr;
    FluentQt::TableView *m_view = nullptr;
    CanRecorder *m_recorder = nullptr;
    FluentQt::CheckBox *m_autoSave = nullptr;
    FluentQt::ToolButton *m_captureFolder = nullptr;
    QWidget *m_fileRowsRow = nullptr;
    FluentQt::SpinBox *m_autoSaveRows = nullptr;
    QString m_captureDirectory;
    QPointer<QFileDialog> m_captureDialog;
    CanMonitorModel *m_model = nullptr;
    CanMonitorProxy *m_sorted = nullptr;
    bool m_heightsQueued = false;
    QTimer *m_refresh = nullptr;
};

} // namespace fhmi
