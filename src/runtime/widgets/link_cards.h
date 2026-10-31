#pragma once

#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"

class QGridLayout;

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

// A panel of the project's Modbus poller (ModbusPoller): its status line and tooltip show the poller's
// status, and its 停止 / 轮询 button pauses and resumes the poller for every card. The project's first card
// sets the poller's fallback slave (defaultSlave), default period (the 轮询ms field, 0 for 500 ms) and
// timeout. 发送 sends the request in the fields through each link's Modbus master as a write, ahead of
// the reads waiting there.
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
    ModbusPoller::Settings settings() const;
    void setStatus(const QString &text, const QString &toolTip = QString());
    // 已发送 with the first frame's bytes and how many links took one, as PyQt UI Designer shows it.
    QString sentStatus() const;
    std::optional<ModbusRequest> fieldRequest();
    void sendFields();
    void manualDone(const ModbusResult &result);
    void showPaused(bool paused);
    void updateValueLabel();

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_status = nullptr;
    FluentQt::LineEdit *m_slave = nullptr;
    FluentQt::ComboBox *m_function = nullptr;
    FluentQt::LineEdit *m_address = nullptr;
    FluentQt::BodyLabel *m_valueLabel = nullptr;
    FluentQt::LineEdit *m_value = nullptr;
    FluentQt::LineEdit *m_poll = nullptr;
    FluentQt::PrimaryPushButton *m_send = nullptr;
    FluentQt::PushButton *m_pollButton = nullptr;
    QList<QByteArray> m_lastSent; // the frames of the last request, one per link that took it
    ModbusExchange m_manual;      // the 发送 request on its way
};

} // namespace fhmi
