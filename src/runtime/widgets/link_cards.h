#pragma once

#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"

class QGridLayout;

namespace FluentQt {
class CaptionLabel;
class CheckBox;
class ComboBox;
class LineEdit;
class PrimaryPushButton;
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
//
// The card also hands the project's Modbus poller its modbusPeriod, modbusTimeout and modbusSlave
// (ModbusPoller::setLinkCard()). While its link is connected, parses Modbus and can send, the status line
// reads <connection> · <poller status>, with the poller's blocks as its tooltip.
class LinkCardBase : public HmiWidget
{
  public:
    LinkCardBase(const WidgetMeta &meta, QWidget *parent);

  protected:
    void paintEvent(QPaintEvent *event) override;
    // Subclasses call these last in theirs, once their link exists.
    void onAttached() override;
    void onDetached() override;

    // The link the card owns now; a TCP card has none while it is not connected.
    virtual Link *currentLink() const = 0;
    // The parser a connecting link uses: the card's own choice, or the project's.
    bool configureParser(Link *link);
    // The button and the status line: `status` describes the connection.
    void setConnectedUi(bool connected, const QString &status);
    // Tells the poller the card's link and settings again: after apply() and whenever the link changes.
    void updatePolling();
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
    FluentQt::CaptionLabel *m_stripStatus = nullptr; // the same status in a single-row strip (serial 单行)
    FluentQt::ComboBox *m_parser = nullptr;
    FluentQt::PrimaryPushButton *m_connect = nullptr;
    QGridLayout *m_grid = nullptr;
    QList<QWidget *> m_fieldLabels;

  private:
    // The card never gets shorter than its visible rows need, so they never overlap.
    void updateMinimumHeight();
    ModbusPoller::Settings pollingSettings() const;
    // The connection text, followed by the poller's status while the link is connected and parses Modbus.
    void showStatus();

    QWidget *m_lastField = nullptr;
    int m_lastRow = -1;
    bool m_connected = false;
    QString m_connection; // what the status line says about the connection
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
    Link *currentLink() const override;

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
    Link *currentLink() const override;

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
    Link *currentLink() const override;

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

} // namespace fhmi
