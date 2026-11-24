#pragma once

#include "comm/modbus_slave.h"
#include "core/can_frame.h"
#include "runtime/hmi_widget.h"

#include <QtGui/QTextBlock>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QTableWidgetItem;
class QTextEdit;
class QTimer;

namespace FluentQt {
class BodyLabel;
class ComboBox;
class CaptionLabel;
class CheckBox;
class LineEdit;
class PushButton;
class StrongBodyLabel;
class TableWidget;
class ToolButton;
} // namespace FluentQt

namespace fhmi {

class ChartHeader;
class ChartHistory;
class ChartPlot;
class CanSignalSet;

// Multi-curve live chart; every curve has its own parse configuration, stored as
// _curve_{i}_{field} properties exactly as PyQt UI Designer stores them. As PyQt UI Designer's
// RealtimeLineChart does, it keeps every sample of the run and plots the last 显示点数 of them, shows each
// curve's value now or at a cursor in a header above the plot, saves the whole run and lays a saved run over
// the live curves for comparison.
class RealtimeChartWidget : public HmiWidget
{
  public:
    RealtimeChartWidget(const WidgetMeta &meta, QWidget *parent);
    ~RealtimeChartWidget() override;

    struct Curve
    {
        QString name;
        std::optional<BindingEntry> binding;
        RoutedSample source;
        DataQuality quality = DataQuality::Waiting;
    };
    // The curve configurations a property set describes.
    static std::vector<Curve> curvesFromProperties(const QJsonObject &props);

    // A sample for a curve, as one arriving from a link is added.
    void appendSample(int curve, double value);
    // What 保存 and 加载 do once a file is chosen; a failure shows in the toolbar, where 实时 or the cursor's
    // sample does.
    void saveHistory(const QString &path);
    void loadReference(const QString &path);
    void clearReference();

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    struct Reference
    {
        QString name;
        QList<double> values;
    };

    void rebuildCurves();
    void setDisplayPoints(int points);
    void applyTheme();
    // The plot and the header from the samples now; data arriving schedules it.
    void refresh();
    // The sample number the live window starts at, which a loaded run is lined up with.
    qint64 windowStart() const;
    void showLiveValues();
    void receiveCurve(int curve, const QVariant &value, const DataSource &source);
    void updateCurve(int curve, const DataPointSample &sample);
    void showCursorValues();
    // The header: the live curves' readings, then the loaded run's at a sample number.
    void showValues(const QStringList &live, qint64 referenceSample);
    // A click on a header entry hides or shows its curve.
    void toggleCurve(int index);

    ChartHeader *m_header = nullptr;
    ChartPlot *m_plot = nullptr;
    FluentQt::CaptionLabel *m_mode = nullptr;
    FluentQt::LineEdit *m_displayEdit = nullptr;
    FluentQt::ToolButton *m_pause = nullptr;
    FluentQt::ToolButton *m_clearReference = nullptr;
    QTimer *m_refreshTimer = nullptr;
    std::vector<Curve> m_curves;
    bool m_hasCanCurves = false; // any curve in CAN信号 mode, so a frame batch is skipped at once when none is
    std::vector<std::unique_ptr<ChartHistory>> m_histories;
    QList<Reference> m_references;
    int m_displayPoints = 200;
    bool m_paused = false;
    bool m_cursorMode = false;
    double m_cursorX = 0;
};

class DataTableWidget : public HmiWidget
{
  public:
    DataTableWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    void onMessage(const Message &message, const DataSource &source);
    void appendRecord(const Message &values);
    void exportCsv();
    bool logging() const;
    bool loggingPoints() const;

    QWidget *m_toolbar = nullptr;
    FluentQt::ToolButton *m_pause = nullptr;
    FluentQt::TableWidget *m_table = nullptr;
    FluentQt::BodyLabel *m_recordHint = nullptr;
    QTimer *m_recordTimer = nullptr;
    QStringList m_recordFields;
    bool m_paused = false;
};

// One area of a single Modbus slave's data table. In a live project, double-clicking edits that slave.
// The designer shows the selected card's initial values, using the same rule for ambiguous sources.
class ModbusSlaveTableWidget : public HmiWidget
{
  public:
    ModbusSlaveTableWidget(const WidgetMeta &meta, QWidget *parent);

    void showDesign(const QJsonObject *cardProperties, bool ambiguous = false, bool editInitialValues = false);
    QString initialValues() const;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    QString table() const;
    const ModbusSlaveStore *store() const;
    void bindSlave();
    void rebuild();
    QString display(quint16 value) const;
    // A value typed in the current format; nullopt for anything it cannot be.
    std::optional<quint16> parse(const QString &text) const;
    void showChange(const Message &message);
    void edited(QTableWidgetItem *item);

    FluentQt::TableWidget *m_table = nullptr;
    std::optional<ModbusSlaveStore> m_design;
    QPointer<ModbusSlave> m_slave;
    QMetaObject::Connection m_slaveConnection;
    bool m_ambiguousSource = false;
    bool m_editInitialValues = false;
    bool m_filling = false;
};

// Time-stamped log of every parsed frame and every link event.
class LogBoxWidget : public HmiWidget
{
  public:
    LogBoxWidget(const WidgetMeta &meta, QWidget *parent);
    void appendLog(const QString &text, const QString &level = QStringLiteral("info"));

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    QWidget *m_toolbar = nullptr;
    FluentQt::ToolButton *m_pause = nullptr;
    QTextEdit *m_output = nullptr;
    bool m_paused = false;
};

// Raw received bytes (text or HEX), what was sent, and link events.
class DataRecvCardWidget : public HmiWidget
{
  public:
    DataRecvCardWidget(const WidgetMeta &meta, QWidget *parent);
    ~DataRecvCardWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    struct PacketRecord;
    struct ReceiveSample
    {
        QByteArray bytes;
        LinkParser::Format format;
        DataSource source;
        bool request = false;
        Message decoded;
        std::optional<CanFrame> can;
        std::shared_ptr<const CanSignalSet> canDefinitions;
    };
    void appendLog(const QString &kind, const QString &text, const DataSource &source, const QColor &color);
    void appendPacket(const QByteArray &bytes, bool received, const DataSource &source, const Message &decoded = {});
    void appendSampleLog(const QString &kind, const QString &text, const QByteArray &bytes, const DataSource &source,
                         const QColor &color);
    std::optional<ReceiveSample> packetSample(const QByteArray &bytes, const DataSource &source, const Link *link,
                                              bool received = true, const Message &decoded = {}) const;
    void queueLog(std::unique_ptr<PacketRecord> record);
    void scheduleLogRefresh();
    void flushLog();
    void discardEvictedSelection(int incomingRows);
    const ReceiveSample *selectedSample() const;
    void updateSampleButton();
    void refreshPacketFormat();
    void refreshCounter();
    void showReceiveSample();
    QString packetText(const PacketRecord &packet, QStringView mode) const;

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_counter = nullptr;
    FluentQt::ComboBox *m_format = nullptr;
    FluentQt::CheckBox *m_autoScroll = nullptr;
    FluentQt::CheckBox *m_wire = nullptr;
    FluentQt::PushButton *m_sample = nullptr;
    QTextEdit *m_output = nullptr;
    QTimer *m_logRefresh = nullptr;
    std::deque<std::unique_ptr<PacketRecord>> m_pendingLog;
    std::optional<ReceiveSample> m_recentSample;
    QTextBlock m_selectedRecord;
    int m_maxLines = 1000;
    int m_receivedCount = 0;
    quint64 m_wireBytes = 0;
};

class DataSendCardWidget : public HmiWidget
{
  public:
    DataSendCardWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void periodicSendChanged(bool running) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void sendInput();
    void sendFields();
    void refreshInputOptions();
    QJsonObject inputProperties() const;

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_target = nullptr;
    FluentQt::LineEdit *m_input = nullptr;
    QWidget *m_textOptions = nullptr;
    QWidget *m_newlineRow = nullptr;
    FluentQt::CheckBox *m_hex = nullptr;
    FluentQt::ComboBox *m_newline = nullptr;
    FluentQt::PushButton *m_send = nullptr;
    FluentQt::PushButton *m_editFields = nullptr;
    QJsonObject m_fieldValues;
    bool m_sendingFields = false;
    bool m_can = false;
};

// The dark terminal of the log and receive views, the same in both themes.
QTextEdit *createTerminal(QWidget *parent);
QString terminalEscape(const QString &text);

} // namespace fhmi
