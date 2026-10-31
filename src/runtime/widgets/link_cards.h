#pragma once

#include "comm/modbus_master.h"
#include "runtime/hmi_widget.h"

#include <functional>

class QGridLayout;
class QHBoxLayout;
class QTimer;

namespace FluentQt {
class BodyLabel;
class CaptionLabel;
class CheckBox;
class ComboBox;
class LineEdit;
class PrimaryPushButton;
class PushButton;
class SpinBox;
class StrongBodyLabel;
class ToolButton;
} // namespace FluentQt

namespace fhmi {

class Link;
class MqttLink;
class SerialLink;
class TcpClientLink;
class TcpServerLink;

// Common frame of the cards that own a link: title, a status line, the parser choice
// and one button that reads 连接, 断开 or 取消. While attached, the card owns its link and
// registers it with the hub; the link closes when the card detaches.
class LinkCardBase : public HmiWidget
{
  public:
    LinkCardBase(const WidgetMeta &meta, QWidget *parent);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void onDetached() override;

    // The parser a connecting link uses: the card's own choice, or the project's.
    bool configureParser(Link *link);
    void setConnectedUi(bool connected, const QString &status);
    void addField(const QString &label, QWidget *field);
    // Empties the field grid; the fields themselves stay owned by the card.
    void clearFields();
    // Puts the button at the right end of the last field row, its field still stretching, then
    // keeps the card from getting shorter than its rows.
    void finishFields();
    // The button at `column` of a single-row strip, then the same minimum height.
    void finishStrip(int column);

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_status = nullptr;
    FluentQt::ComboBox *m_parser = nullptr;
    FluentQt::PrimaryPushButton *m_connect = nullptr;
    QGridLayout *m_grid = nullptr;
    QList<QWidget *> m_fieldLabels;

  private:
    // The card never gets shorter than its visible rows need, so they never overlap.
    void updateMinimumHeight();

    QWidget *m_lastField = nullptr;
    int m_lastRow = -1;
};

class SerialConfigCardWidget : public LinkCardBase
{
  public:
    SerialConfigCardWidget(const WidgetMeta &meta, QWidget *parent);
    ~SerialConfigCardWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    void refreshPorts();
    void toggleConnection();

    QWidget *m_portRow = nullptr;
    FluentQt::ComboBox *m_port = nullptr;
    FluentQt::ToolButton *m_refresh = nullptr;
    FluentQt::ComboBox *m_baud = nullptr;
    FluentQt::ComboBox *m_dataBits = nullptr;
    FluentQt::ComboBox *m_parity = nullptr;
    FluentQt::ComboBox *m_stopBits = nullptr;
    FluentQt::ComboBox *m_flow = nullptr;
    SerialLink *m_link = nullptr;
};

class TcpConfigCardWidget : public LinkCardBase
{
  public:
    TcpConfigCardWidget(const WidgetMeta &meta, QWidget *parent);
    ~TcpConfigCardWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    void toggleConnection();
    void dropLink();

    FluentQt::ComboBox *m_mode = nullptr;
    FluentQt::LineEdit *m_host = nullptr;
    FluentQt::SpinBox *m_port = nullptr;
    Link *m_link = nullptr;
};

class MqttConfigCardWidget : public LinkCardBase
{
  public:
    MqttConfigCardWidget(const WidgetMeta &meta, QWidget *parent);
    ~MqttConfigCardWidget() override;

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    void toggleConnection();
    void refreshState();
    QWidget *row(std::initializer_list<QPair<QWidget *, int>> widgets);

    FluentQt::LineEdit *m_host = nullptr;
    FluentQt::SpinBox *m_port = nullptr;
    FluentQt::LineEdit *m_clientId = nullptr;
    FluentQt::LineEdit *m_username = nullptr;
    FluentQt::LineEdit *m_password = nullptr;
    FluentQt::LineEdit *m_subscribe = nullptr;
    FluentQt::LineEdit *m_publish = nullptr;
    FluentQt::ComboBox *m_qos = nullptr;
    FluentQt::CheckBox *m_tls = nullptr;
    QWidget *m_serverRow = nullptr;
    QWidget *m_loginRow = nullptr;
    QWidget *m_qosRow = nullptr;
    MqttLink *m_link = nullptr;
};

// Reads and writes Modbus over the links that parse it. Polling has two modes (pollMode): 自动 plans
// read blocks from the project's Modbus bindings and chart curves and reads each at its period; 轮询表
// reads the requests of pollList in rounds. Every request queues in its link's ModbusMaster, so writes
// from other widgets go out between two reads.
class ModbusConfigCardWidget : public HmiWidget
{
  public:
    ModbusConfigCardWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    // A request of this card waiting for every link that queued it; `complete` gets the first success,
    // else the first failure.
    struct Pending
    {
        int token = 0;
        std::function<void(const ModbusResult &)> complete;
        QList<QPointer<ModbusMaster>> waiting;
        QList<const ModbusMaster *> reported; // results that came in before `waiting` was known
        bool armed = false;
        std::optional<ModbusResult> result;
    };

    bool autoMode() const;
    // Whether a connected link parses Modbus, so replies to the requests sent can be recognised.
    // Without one, each poll-table tick sends the next request without waiting, as the card always did.
    bool expectsReplies() const;
    // The connected links that parse Modbus and can send now: where automatic polling reads.
    QList<Link *> modbusLinks() const;
    void setStatus(const QString &text);
    // 已发送 with the first frame's bytes and how many links took one, as PyQt UI Designer shows it.
    QString sentStatus() const;
    std::optional<ModbusRequest> fieldRequest();
    // The 轮询ms field; automatic polling takes 0 as 500.
    int defaultPeriod() const;

    // Queues `request` on the links; `pending` then waits for their replies. `announce` shows 已发送 once
    // the links took it. False after explaining why no link took it.
    bool queue(const ModbusRequest &request, ModbusMaster::Priority priority, std::optional<Pending> &pending,
               std::function<void(const ModbusResult &)> complete, bool announce, bool modbusLinksOnly = false);
    void report(std::optional<Pending> &pending, int token, const ModbusResult &result);
    // Completes `pending` once no link it waits for is left (a link that went away never reports).
    void finishIfSettled(std::optional<Pending> &pending);
    void sendFields();
    void manualDone(const ModbusResult &result);

    void togglePolling();
    void startPolling(bool automatic);
    void stopPolling();
    void maybeAutoStart(Link *link);
    void pollTick();

    // 轮询表
    void tableTick();
    void sendNextInRound();
    void tableDone(int item, const ModbusResult &result);

    // 自动
    void schedule();
    void replan();
    void blockDone(int block, int plan, const ModbusResult &result);
    void roundDone();
    void setAutoStatus(const QString &text);

    void updateValueLabel();

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_modeHint = nullptr;
    FluentQt::CaptionLabel *m_status = nullptr;
    FluentQt::LineEdit *m_slave = nullptr;
    FluentQt::ComboBox *m_function = nullptr;
    FluentQt::LineEdit *m_address = nullptr;
    FluentQt::BodyLabel *m_valueLabel = nullptr;
    FluentQt::LineEdit *m_value = nullptr;
    FluentQt::LineEdit *m_poll = nullptr;
    FluentQt::PrimaryPushButton *m_send = nullptr;
    FluentQt::PushButton *m_pollButton = nullptr;
    QTimer *m_timer = nullptr; // poll ticks
    QList<QByteArray> m_lastSent; // the frames of the last request, one per link that took it
    int m_token = 0;
    std::optional<Pending> m_read;   // the polled read on its way
    std::optional<Pending> m_manual; // the 发送 request on its way
    bool m_autoPolling = false;

    QList<ModbusRequest> m_table; // the reads of each round; empty: the request in the fields
    QList<ModbusRequest> m_round; // still to send in this round
    int m_roundSize = 0;
    QList<QPair<int, QString>> m_roundProblems; // (1-based entry, problem)
    int m_next = 0; // the request a poll tick sends when nothing waits for replies

    QList<ModbusBlock> m_blocks;
    QList<qint64> m_due; // per block: when it is read next
    QList<std::optional<ModbusResult>> m_blockResults; // per block, in this round
    int m_plan = 0; // counts plans, so a block read under an older plan does not count in this round
};

} // namespace fhmi
