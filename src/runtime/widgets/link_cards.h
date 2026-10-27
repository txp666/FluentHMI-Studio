#pragma once

#include "runtime/hmi_widget.h"

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
// and a connect button. While attached, the card owns its link and registers it with
// the hub; the link closes when the card detaches.
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
    // The connect button sits under the fields, or at the end of a single-row grid.
    void placeConnectButton(bool inGrid, int column = 0);

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_status = nullptr;
    FluentQt::ComboBox *m_parser = nullptr;
    FluentQt::PrimaryPushButton *m_connect = nullptr;
    QGridLayout *m_grid = nullptr;
    QHBoxLayout *m_buttonRow = nullptr;
    QList<QWidget *> m_fieldLabels;
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
    bool sendOnce();
    void togglePolling();
    void stopPolling();
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
    QTimer *m_timer = nullptr;
};

} // namespace fhmi
