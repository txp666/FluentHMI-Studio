#include "runtime/widgets/link_cards.h"

#include "comm/mqtt_link.h"
#include "comm/serial_link.h"
#include "comm/tcp_link.h"
#include "runtime/runtime_context.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/display_widgets.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>

#include <QtCore/QHash>
#include <QtCore/QTimer>
#include <QtGui/QIntValidator>
#include <QtGui/QPainter>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {

namespace {

const QString kProjectParser = QStringLiteral("项目设置");

void selectText(FluentQt::ComboBox *combo, const QString &text)
{
    const int index = combo->findText(text);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
}

FluentQt::ComboBox *comboOf(QWidget *parent, const QStringList &items)
{
    auto *combo = new FluentQt::ComboBox(parent);
    combo->addItems(items);
    return combo;
}

QStringList enumOptions(const WidgetMeta &meta, const char *name)
{
    const PropDef *def = meta.prop(QLatin1String(name));
    return def ? def->options : QStringList();
}

} // namespace

// ---------------------------------------------------------------------------
// LinkCardBase
// ---------------------------------------------------------------------------

LinkCardBase::LinkCardBase(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    m_title = new FluentQt::StrongBodyLabel(this);
    m_status = new FluentQt::CaptionLabel(this);
    header->addWidget(m_title);
    header->addStretch();
    header->addWidget(m_status);
    m_grid = new QGridLayout;
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(6);
    m_grid->setColumnStretch(1, 1);
    m_parser = comboOf(this, enumOptions(meta, "defaultParser"));
    m_connect = new FluentQt::PrimaryPushButton(this);
    m_connect->setText(tr("连接"));
    m_buttonRow = new QHBoxLayout;
    m_buttonRow->addStretch();
    m_buttonRow->addWidget(m_connect);
    layout->addLayout(header);
    layout->addLayout(m_grid);
    layout->addStretch();
    layout->addLayout(m_buttonRow);
    setConnectedUi(false, tr("未连接"));
}

void LinkCardBase::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void LinkCardBase::onDetached()
{
    setConnectedUi(false, tr("未连接"));
}

bool LinkCardBase::configureParser(Link *link)
{
    CommHub *hub = context()->hub();
    QString type = m_parser->currentText();
    QJsonObject config;
    // The card's own frame format applies once it picks its own mode; left empty, a card on
    // the project's mode still uses the project's format.
    const QJsonObject own = prop("parserConfig").toObject();
    if (type.isEmpty() || type == kProjectParser) {
        type = hub->defaultParserType();
        config = hub->defaultParserConfig();
    } else if (!own.isEmpty()) {
        config = own;
    } else if (type == hub->defaultParserType()) {
        config = hub->defaultParserConfig();
    }
    QString error;
    if (!link->setParser(type, config, &error)) {
        context()->notify(tr("解析模式无效"), error, NotifyType::Error, 3000);
        return false;
    }
    return true;
}

void LinkCardBase::setConnectedUi(bool connected, const QString &status)
{
    m_connect->setText(connected ? tr("断开") : tr("连接"));
    m_status->setText(status);
    m_status->setTextColor(connected ? QColor(0x10, 0x7C, 0x10) : QColor(0x80, 0x80, 0x80),
                           connected ? QColor(0x6C, 0xCB, 0x5F) : QColor(0xA0, 0xA0, 0xA0));
}

void LinkCardBase::addField(const QString &label, QWidget *field)
{
    const int row = m_grid->rowCount();
    auto *caption = new FluentQt::BodyLabel(label, this);
    m_grid->addWidget(caption, row, 0);
    m_grid->addWidget(field, row, 1);
    caption->show();
    field->show();
    m_fieldLabels.append(caption);
}

void LinkCardBase::clearFields()
{
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QWidget *widget = item->widget(); widget && widget != m_connect) {
            widget->hide();
        }
        delete item;
    }
    qDeleteAll(m_fieldLabels);
    m_fieldLabels.clear();
}

void LinkCardBase::placeConnectButton(bool inGrid, int column)
{
    m_grid->removeWidget(m_connect);
    m_buttonRow->removeWidget(m_connect);
    if (inGrid) {
        m_grid->addWidget(m_connect, 0, column);
    } else {
        m_buttonRow->addWidget(m_connect);
    }
    m_connect->show();
}

// ---------------------------------------------------------------------------
// SerialConfigCard
// ---------------------------------------------------------------------------

SerialConfigCardWidget::SerialConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : LinkCardBase(meta, parent)
{
    m_portRow = new QWidget(this);
    auto *portLayout = new QHBoxLayout(m_portRow);
    portLayout->setContentsMargins(0, 0, 0, 0);
    portLayout->setSpacing(4);
    m_port = new FluentQt::ComboBox(m_portRow);
    m_port->setPlaceholderText(tr("选择串口"));
    m_refresh = new FluentQt::ToolButton(m_portRow);
    m_refresh->setIcon(FluentQt::icon(FluentQt::FluentIcon::Sync));
    keepSquareBeside(m_refresh, m_port);
    portLayout->addWidget(m_port, 1);
    portLayout->addWidget(m_refresh);
    m_baud = comboOf(this, enumOptions(meta, "defaultBaud"));
    m_dataBits = comboOf(this, enumOptions(meta, "defaultDatabits"));
    m_parity = comboOf(this, enumOptions(meta, "defaultParity"));
    m_stopBits = comboOf(this, enumOptions(meta, "defaultStopbits"));
    m_flow = comboOf(this, enumOptions(meta, "defaultFlow"));
    connect(m_refresh, &QToolButton::clicked, this, &SerialConfigCardWidget::refreshPorts);
}

SerialConfigCardWidget::~SerialConfigCardWidget()
{
    detach();
}

void SerialConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    selectText(m_baud, text("defaultBaud"));
    selectText(m_dataBits, text("defaultDatabits"));
    selectText(m_parity, text("defaultParity"));
    selectText(m_stopBits, text("defaultStopbits"));
    selectText(m_flow, text("defaultFlow"));
    selectText(m_parser, text("defaultParser"));

    clearFields();
    for (QWidget *w : {static_cast<QWidget *>(m_dataBits), static_cast<QWidget *>(m_parity),
                       static_cast<QWidget *>(m_stopBits), static_cast<QWidget *>(m_flow),
                       static_cast<QWidget *>(m_parser)}) {
        w->hide();
    }
    const QString mode = text("displayMode");
    const bool singleRow = mode == QStringLiteral("单行");
    m_title->setVisible(!singleRow);
    m_status->setVisible(!singleRow);
    auto *layout = static_cast<QVBoxLayout *>(this->layout());
    if (singleRow) {
        // Port, baud rate and the button on one line, for a toolbar-like strip.
        layout->setContentsMargins(8, 4, 8, 4);
        m_grid->addWidget(m_portRow, 0, 0);
        m_grid->addWidget(m_baud, 0, 1);
        m_portRow->show();
        m_baud->show();
        placeConnectButton(true, 2);
        return;
    }
    layout->setContentsMargins(14, 10, 14, 12);
    placeConnectButton(false);
    addField(tr("串口"), m_portRow);
    addField(tr("波特率"), m_baud);
    if (mode != QStringLiteral("简洁")) {
        addField(tr("数据位"), m_dataBits);
        addField(tr("校验位"), m_parity);
        addField(tr("停止位"), m_stopBits);
        addField(tr("流控制"), m_flow);
        addField(tr("解析模式"), m_parser);
    }
}

void SerialConfigCardWidget::refreshPorts()
{
    const QString current = m_port->currentText();
    m_port->clear();
    m_port->addItems(SerialLink::availablePorts());
    selectText(m_port, current);
}

void SerialConfigCardWidget::onAttached()
{
    m_link = new SerialLink(this);
    context()->hub()->addLink(m_link);
    refreshPorts();
    live(connect(m_connect, &QPushButton::clicked, this, &SerialConfigCardWidget::toggleConnection));
    live(connect(m_link, &Link::connectedChanged, this, [this](bool connected) {
        setConnectedUi(connected, connected ? m_link->description() : tr("未连接"));
    }));
    live(connect(m_link, &Link::errorOccurred, this, [this](const QString &message) {
        context()->notify(tr("串口错误"), message, NotifyType::Error, 3000);
    }));
}

void SerialConfigCardWidget::onDetached()
{
    if (m_link) {
        m_link->close();
        context()->hub()->removeLink(m_link);
        delete m_link;
        m_link = nullptr;
    }
    LinkCardBase::onDetached();
}

void SerialConfigCardWidget::toggleConnection()
{
    if (m_link->isConnected()) {
        m_link->close();
        return;
    }
    SerialSettings settings;
    settings.portName = m_port->currentText();
    if (settings.portName.isEmpty()) {
        context()->notify(tr("提示"), tr("请先选择串口"), NotifyType::Warning);
        return;
    }
    settings.baudRate = m_baud->currentText().toInt();
    settings.dataBits = m_dataBits->currentText();
    settings.parity = m_parity->currentText();
    settings.stopBits = m_stopBits->currentText();
    settings.flowControl = m_flow->currentText();
    if (!configureParser(m_link)) {
        return;
    }
    QString error;
    if (!m_link->open(settings, &error)) {
        context()->notify(tr("打开串口失败"), error, NotifyType::Error, 3000);
    }
}

// ---------------------------------------------------------------------------
// TcpConfigCard
// ---------------------------------------------------------------------------

TcpConfigCardWidget::TcpConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : LinkCardBase(meta, parent)
{
    m_mode = comboOf(this, enumOptions(meta, "defaultMode"));
    m_host = new FluentQt::LineEdit(this);
    m_port = new FluentQt::SpinBox(this);
    m_port->setRange(1, 65535);
    connect(m_mode, &FluentQt::ComboBox::currentIndexChanged, this, [this]() {
        m_host->setPlaceholderText(m_mode->currentIndex() == 1 ? QStringLiteral("0.0.0.0") : QString());
    });
}

TcpConfigCardWidget::~TcpConfigCardWidget()
{
    detach();
}

void TcpConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    selectText(m_mode, text("defaultMode"));
    m_host->setText(text("defaultHost"));
    m_port->setValue(integer("defaultPort"));
    selectText(m_parser, text("defaultParser"));
    clearFields();
    m_parser->hide();
    m_mode->hide();
    addField(tr("模式"), m_mode);
    addField(tr("地址"), m_host);
    addField(tr("端口"), m_port);
    if (text("displayMode") != QStringLiteral("简洁")) {
        addField(tr("解析模式"), m_parser);
    }
}

void TcpConfigCardWidget::onAttached()
{
    live(connect(m_connect, &QPushButton::clicked, this, &TcpConfigCardWidget::toggleConnection));
}

void TcpConfigCardWidget::onDetached()
{
    dropLink();
    LinkCardBase::onDetached();
}

void TcpConfigCardWidget::dropLink()
{
    if (!m_link) {
        return;
    }
    m_link->close();
    if (context()) {
        context()->hub()->removeLink(m_link);
    }
    delete m_link;
    m_link = nullptr;
}

void TcpConfigCardWidget::toggleConnection()
{
    if (m_link) {
        // Disconnect, or cancel a pending connection attempt.
        dropLink();
        setConnectedUi(false, tr("未连接"));
        m_mode->setEnabled(true);
        return;
    }
    const bool server = m_mode->currentIndex() == 1;
    Link *link = server ? static_cast<Link *>(new TcpServerLink(this)) : new TcpClientLink(this);
    if (!configureParser(link)) {
        delete link;
        return;
    }
    m_link = link;
    context()->hub()->addLink(link);
    connect(link, &Link::connectedChanged, this, [this, link](bool connected) {
        setConnectedUi(connected, connected ? link->description() : tr("未连接"));
        if (!connected && m_link == link && qobject_cast<TcpClientLink *>(link)) {
            // A client that lost its device is done; the next click connects afresh.
            QTimer::singleShot(0, this, [this, link]() {
                if (m_link == link) {
                    dropLink();
                    m_mode->setEnabled(true);
                }
            });
        }
    });
    connect(link, &Link::errorOccurred, this, [this](const QString &message) {
        context()->notify(tr("TCP 错误"), message, NotifyType::Error, 3000);
    });
    m_mode->setEnabled(false);
    const quint16 port = quint16(m_port->value());
    if (server) {
        auto *serverLink = static_cast<TcpServerLink *>(link);
        connect(serverLink, &TcpServerLink::clientCountChanged, this, [this, serverLink](int count) {
            setConnectedUi(true, tr("%1 · %2 台设备").arg(serverLink->description()).arg(count));
        });
        QString error;
        if (!serverLink->listen(m_host->text(), port, &error)) {
            context()->notify(tr("监听失败"), error, NotifyType::Error, 3000);
            dropLink();
            m_mode->setEnabled(true);
        }
    } else {
        m_connect->setText(tr("取消"));
        m_status->setText(tr("连接中..."));
        static_cast<TcpClientLink *>(link)->connectTo(m_host->text().trimmed(), port);
    }
}

// ---------------------------------------------------------------------------
// MqttConfigCard
// ---------------------------------------------------------------------------

MqttConfigCardWidget::MqttConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : LinkCardBase(meta, parent)
{
    m_host = new FluentQt::LineEdit(this);
    m_host->setPlaceholderText(tr("broker 地址"));
    m_port = new FluentQt::SpinBox(this);
    m_port->setRange(1, 65535);
    m_clientId = new FluentQt::LineEdit(this);
    m_clientId->setPlaceholderText(tr("留空自动生成"));
    m_username = new FluentQt::LineEdit(this);
    m_username->setPlaceholderText(tr("可选"));
    m_password = new FluentQt::LineEdit(this);
    m_password->setPlaceholderText(tr("可选"));
    m_password->setEchoMode(QLineEdit::Password);
    m_subscribe = new FluentQt::LineEdit(this);
    m_subscribe->setPlaceholderText(tr("多个主题用逗号分隔，支持 + 和 #"));
    m_publish = new FluentQt::LineEdit(this);
    m_publish->setPlaceholderText(tr("命令发往的主题"));
    m_qos = comboOf(this, enumOptions(meta, "defaultQos"));
    m_tls = new FluentQt::CheckBox(QStringLiteral("TLS"), this);

    m_serverRow = row({{m_host, 3}, {m_port, 1}});
    m_loginRow = row({{m_username, 1}, {new FluentQt::CaptionLabel(tr("密码"), this), 0}, {m_password, 1}});
    m_qosRow = row({{m_qos, 1}, {m_tls, 0}});
}

MqttConfigCardWidget::~MqttConfigCardWidget()
{
    detach();
}

QWidget *MqttConfigCardWidget::row(std::initializer_list<QPair<QWidget *, int>> widgets)
{
    auto *container = new QWidget(this);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (const auto &entry : widgets) {
        entry.first->setParent(container);
        layout->addWidget(entry.first, entry.second);
    }
    container->hide();
    return container;
}

void MqttConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    m_host->setText(text("defaultHost"));
    m_port->setValue(integer("defaultPort"));
    m_clientId->setText(text("defaultClientId"));
    m_username->setText(text("defaultUsername"));
    m_password->setText(text("defaultPassword"));
    m_subscribe->setText(text("defaultSubscribe"));
    m_publish->setText(text("defaultPublish"));
    selectText(m_qos, text("defaultQos"));
    m_tls->setChecked(flag("defaultTls"));
    selectText(m_parser, text("defaultParser"));

    clearFields();
    for (QWidget *w : {static_cast<QWidget *>(m_clientId), m_loginRow, m_qosRow, static_cast<QWidget *>(m_parser)}) {
        w->hide();
    }
    const bool full = text("displayMode") != QStringLiteral("简洁");
    addField(tr("服务器"), m_serverRow);
    if (full) {
        addField(tr("客户端ID"), m_clientId);
        addField(tr("用户名"), m_loginRow);
    }
    addField(tr("订阅主题"), m_subscribe);
    addField(tr("发布主题"), m_publish);
    if (full) {
        addField(QStringLiteral("QoS"), m_qosRow);
        addField(tr("解析模式"), m_parser);
    }
}

void MqttConfigCardWidget::onAttached()
{
    m_link = new MqttLink(this);
    context()->hub()->addLink(m_link);
    live(connect(m_connect, &QPushButton::clicked, this, &MqttConfigCardWidget::toggleConnection));
    live(connect(m_link, &Link::connectedChanged, this, &MqttConfigCardWidget::refreshState));
    live(connect(m_link, &Link::errorOccurred, this, [this](const QString &message) {
        context()->notify(tr("MQTT"), message, NotifyType::Error, 3000);
        refreshState();
    }));
}

void MqttConfigCardWidget::onDetached()
{
    if (m_link) {
        m_link->close();
        context()->hub()->removeLink(m_link);
        delete m_link;
        m_link = nullptr;
    }
    LinkCardBase::onDetached();
    refreshState();
}

void MqttConfigCardWidget::toggleConnection()
{
    if (m_link->isActive()) {
        // Disconnect, or cancel connecting / reconnecting.
        m_link->close();
        refreshState();
        return;
    }
    if (!configureParser(m_link)) {
        return;
    }
    MqttSettings settings;
    settings.host = m_host->text();
    settings.port = m_port->value();
    settings.clientId = m_clientId->text();
    settings.username = m_username->text();
    settings.password = m_password->text();
    settings.topics = Mqtt::topics(m_subscribe->text());
    settings.publishTopic = m_publish->text();
    settings.qos = m_qos->currentText().toInt();
    settings.tls = m_tls->isChecked();
    QString error;
    if (!m_link->connectTo(settings, &error)) {
        context()->notify(tr("MQTT"), error, NotifyType::Error, 3000);
    }
    refreshState();
}

void MqttConfigCardWidget::refreshState()
{
    const bool active = m_link && m_link->isActive();
    const bool connected = m_link && m_link->isConnected();
    for (QWidget *w : {static_cast<QWidget *>(m_host), static_cast<QWidget *>(m_port),
                       static_cast<QWidget *>(m_clientId), static_cast<QWidget *>(m_username),
                       static_cast<QWidget *>(m_password), static_cast<QWidget *>(m_subscribe),
                       static_cast<QWidget *>(m_publish), static_cast<QWidget *>(m_qos),
                       static_cast<QWidget *>(m_tls), static_cast<QWidget *>(m_parser)}) {
        w->setEnabled(!active);
    }
    if (connected) {
        setConnectedUi(true, m_link->description());
    } else if (active) {
        setConnectedUi(false, m_link->isReconnecting() ? tr("重连中...") : tr("连接中..."));
        m_connect->setText(tr("取消"));
    } else {
        setConnectedUi(false, tr("未连接"));
    }
}

// ---------------------------------------------------------------------------
// ModbusConfigCard
// ---------------------------------------------------------------------------

namespace {

// Why a Modbus reply is not a success: the exception code a device answered with, or empty.
QString modbusProblem(const Message &message)
{
    if (!message.contains(QStringLiteral("error"))) {
        return {};
    }
    static const QHash<int, QString> names = {
        {1, QStringLiteral("非法功能码")},   {2, QStringLiteral("非法数据地址")},
        {3, QStringLiteral("非法数据值")},   {4, QStringLiteral("从站设备故障")},
        {5, QStringLiteral("已确认，处理中")}, {6, QStringLiteral("从站忙")},
        {10, QStringLiteral("网关路径不可用")}, {11, QStringLiteral("网关目标设备无响应")},
    };
    const int code = message.value(QStringLiteral("error")).toInt();
    const QString hex = QStringLiteral("%1").arg(code, 2, 16, QLatin1Char('0')).toUpper();
    return QStringLiteral("异常 %1 %2").arg(hex, names.value(code)).trimmed();
}

} // namespace

ModbusConfigCardWidget::ModbusConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    // The layout of PyQt UI Designer's card: plain number fields fit its 380 x 180 default,
    // where spin boxes would leave no room for the digits.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    m_title = new FluentQt::StrongBodyLabel(this);
    m_status = new FluentQt::CaptionLabel(tr("就绪"), this);
    m_status->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xb0, 0xb0, 0xb0));
    // A long status takes only the room the title leaves; the tooltip has all of it.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(m_title);
    header->addWidget(m_status, 1);

    const auto number = [this](int lo, int hi, const QString &placeholder) {
        auto *edit = new FluentQt::LineEdit(this);
        edit->setValidator(new QIntValidator(lo, hi, edit));
        edit->setPlaceholderText(placeholder);
        return edit;
    };
    const auto label = [this](const QString &text) {
        auto *caption = new FluentQt::BodyLabel(text, this);
        caption->setFixedWidth(56);
        return caption;
    };
    m_slave = number(1, 247, QStringLiteral("1"));
    m_function = comboOf(this, enumOptions(meta, "defaultFunc"));
    m_address = number(0, 65535, QStringLiteral("0"));
    m_valueLabel = label(tr("数量"));
    m_value = new FluentQt::LineEdit(this); // a count, a register value or a typed FC 16 value
    m_value->setPlaceholderText(QStringLiteral("1"));
    m_poll = number(0, 60000, tr("0=不轮询"));
    m_send = new FluentQt::PrimaryPushButton(this);
    m_send->setText(tr("发送"));
    m_pollButton = new FluentQt::PushButton(this);
    m_pollButton->setText(tr("轮询"));

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->addWidget(label(tr("从站")), 0, 0);
    grid->addWidget(m_slave, 0, 1);
    grid->addWidget(label(tr("功能码")), 0, 2);
    grid->addWidget(m_function, 0, 3);
    grid->addWidget(label(tr("地址")), 1, 0);
    grid->addWidget(m_address, 1, 1);
    grid->addWidget(m_valueLabel, 1, 2);
    grid->addWidget(m_value, 1, 3);
    grid->addWidget(label(tr("轮询ms")), 2, 0);
    grid->addWidget(m_poll, 2, 1);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_send, 1);
    buttons->addWidget(m_pollButton, 1);
    grid->addLayout(buttons, 2, 2, 1, 2);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
    layout->addLayout(header);
    layout->addLayout(grid);
    layout->addStretch();
    m_timer = new QTimer(this);
    m_replyTimer = new QTimer(this);
    m_replyTimer->setSingleShot(true);
    connect(m_function, &FluentQt::ComboBox::currentIndexChanged, this, &ModbusConfigCardWidget::updateValueLabel);
}

void ModbusConfigCardWidget::updateValueLabel()
{
    const int func = CommandEncoder::functionCode(m_function->currentText());
    m_valueLabel->setText(func == 0x05 || func == 0x06 || func == 0x10 ? tr("写入值") : tr("数量"));
}

void ModbusConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    m_slave->setText(QString::number(integer("defaultSlave")));
    selectText(m_function, text("defaultFunc"));
    m_address->setText(QString::number(integer("defaultAddr")));
    m_value->setText(text("defaultValue")); // older projects hold an int; text() reads both
    m_poll->setText(QString::number(integer("defaultPoll")));
    updateValueLabel();
}

void ModbusConfigCardWidget::onAttached()
{
    live(connect(m_send, &QPushButton::clicked, this, [this]() {
        if (const auto request = fieldRequest(); request && sendRequest(*request)) {
            // "已发送" only means every byte was written; the reply, or its absence, follows.
            setStatus(sentStatus());
        }
    }));
    live(connect(m_pollButton, &QPushButton::clicked, this, &ModbusConfigCardWidget::togglePolling));
    live(connect(m_timer, &QTimer::timeout, this, &ModbusConfigCardWidget::pollTick));
    live(connect(m_replyTimer, &QTimer::timeout, this, [this]() {
        m_waiting.reset();
        replied(tr("无响应"));
    }));
    live(connect(context()->hub(), &CommHub::messageReceived, this, &ModbusConfigCardWidget::onMessage));
    live(connect(context()->hub(), &CommHub::linkConnected, this,
                 [this](const QString &link) { setStatus(tr("已连接: %1").arg(link)); }));
    live(connect(context()->hub(), &CommHub::connectionStateChanged, this, [this](bool any) {
        if (!any) {
            stopPolling();
            setStatus(tr("未连接"));
        }
    }));
}

void ModbusConfigCardWidget::onDetached()
{
    stopPolling();
    setStatus(tr("就绪"));
}

std::optional<ModbusRequest> ModbusConfigCardWidget::fieldRequest()
{
    bool ok[2] = {};
    const int slave = m_slave->text().toInt(&ok[0]);
    const int address = m_address->text().toInt(&ok[1]);
    QString error = tr("从站、地址和数值必须是整数");
    std::optional<ModbusRequest> request;
    if (ok[0] && ok[1]) {
        request = CommandEncoder::modbusRequest(slave, CommandEncoder::functionCode(m_function->currentText()),
                                                address, m_value->text(), text("mbDataType"), text("mbRegOrder"),
                                                &error);
    }
    if (!request) {
        context()->notify(QStringLiteral("Modbus"), error, NotifyType::Error);
        setStatus(tr("参数无效"));
        stopPolling();
    }
    return request;
}

void ModbusConfigCardWidget::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setToolTip(text);
}

QString ModbusConfigCardWidget::sentStatus() const
{
    QString status = tr("已发送: %1").arg(QString::fromLatin1(m_lastSent.value(0).toHex(' ')).toUpper());
    if (m_lastSent.size() > 1) {
        status += tr("（%1 条链路）").arg(m_lastSent.size());
    }
    return status;
}

bool ModbusConfigCardWidget::expectsReplies() const
{
    for (Link *link : context()->hub()->connectedLinks()) {
        const QString type = link->parserType();
        if (type == QLatin1String("modbus") || type == QLatin1String("modbus_tcp") ||
            type == QLatin1String("modbus_ascii")) {
            return true;
        }
    }
    return false;
}

bool ModbusConfigCardWidget::sendRequest(const ModbusRequest &request)
{
    EncodedCommand command;
    command.kind = EncodedCommand::Kind::Modbus;
    command.modbus = request;
    m_lastSent.clear();
    if (!context()->send(CommandResult::ok(command), &m_lastSent)) {
        stopPolling(); // a failed write stops polling
        setStatus(tr("发送失败"));
        return false;
    }
    if (expectsReplies()) {
        m_waiting = request;
        m_replyTimer->start(std::max(10, integer("timeout")));
    }
    return true;
}

void ModbusConfigCardWidget::onMessage(const Message &message)
{
    if (!m_waiting) {
        return;
    }
    const qint64 fc = message.value(QStringLiteral("fc")).toLongLong();
    if (message.value(QStringLiteral("slave")).toLongLong() != m_waiting->slave ||
        (fc != m_waiting->func && fc != (m_waiting->func | 0x80))) {
        return;
    }
    // Reads name their start address, writes echo it. Only an exception to a write names neither;
    // a read reply without a start (its count differs) answers some other request.
    const QVariant where = message.contains(QStringLiteral("start")) ? message.value(QStringLiteral("start"))
                                                                     : message.value(QStringLiteral("address"));
    if (where.isValid() ? where.toLongLong() != m_waiting->address : fc < 0x80) {
        return;
    }
    m_replyTimer->stop();
    m_waiting.reset();
    replied(modbusProblem(message));
}

void ModbusConfigCardWidget::replied(const QString &problem)
{
    if (!m_timer->isActive()) {
        if (problem.isEmpty()) {
            setStatus(tr("设备已响应"));
        } else if (problem == tr("无响应")) {
            setStatus(tr("无响应（%1 ms 超时）").arg(std::max(10, integer("timeout"))));
        } else {
            setStatus(tr("设备返回%1").arg(problem));
        }
        return;
    }
    if (!problem.isEmpty()) {
        m_roundProblems.append({m_roundSize - int(m_round.size()), problem});
    }
    if (!m_round.isEmpty()) {
        sendRequest(m_round.takeFirst());
        return;
    }
    const int good = m_roundSize - int(m_roundProblems.size());
    if (m_roundProblems.isEmpty()) {
        setStatus(tr("轮询正常 · %1/%2").arg(good).arg(m_roundSize));
    } else {
        const auto &[number, first] = m_roundProblems.first();
        const QString where = m_roundSize > 1 ? tr("第 %1 条").arg(number) : QString();
        setStatus(QStringLiteral("%1%2 · %3/%4").arg(where, first).arg(good).arg(m_roundSize));
    }
}

void ModbusConfigCardWidget::pollTick()
{
    if (m_waiting) {
        return; // the last request is still waiting for its reply
    }
    if (!expectsReplies()) { // nothing to wait for: one request per tick, in turn
        std::optional<ModbusRequest> request;
        if (m_table.isEmpty()) {
            request = fieldRequest();
        } else {
            m_next %= int(m_table.size());
            request = m_table.at(m_next++);
        }
        if (request && sendRequest(*request)) {
            setStatus(sentStatus());
        }
        return;
    }
    if (m_table.isEmpty()) {
        const auto request = fieldRequest();
        if (!request) {
            return;
        }
        m_round = {*request};
    } else {
        m_round = m_table;
    }
    m_roundSize = int(m_round.size());
    m_roundProblems.clear();
    sendRequest(m_round.takeFirst());
}

void ModbusConfigCardWidget::stopPolling()
{
    m_timer->stop();
    m_replyTimer->stop();
    m_waiting.reset();
    m_round.clear();
    m_pollButton->setText(tr("轮询"));
    m_send->setEnabled(true);
}

void ModbusConfigCardWidget::togglePolling()
{
    if (m_timer->isActive()) {
        stopPolling();
        return;
    }
    const int interval = m_poll->text().toInt();
    if (interval <= 0) {
        context()->notify(QStringLiteral("Modbus"), tr("请先设置轮询间隔"), NotifyType::Warning);
        return;
    }
    QString error;
    const auto table = CommandEncoder::modbusPollTable(text("pollList"), &error);
    if (!table) {
        context()->notify(QStringLiteral("Modbus"), error, NotifyType::Error);
        setStatus(error);
        return;
    }
    m_table = *table;
    m_replyTimer->stop();
    m_waiting.reset();
    m_next = 0;
    m_timer->start(interval);
    m_pollButton->setText(tr("停止"));
    m_send->setEnabled(false); // a request of its own would take the place of the one polling waits for
    if (!m_table.isEmpty()) {
        setStatus(tr("轮询中 · %1 条").arg(m_table.size()));
    }
    pollTick(); // the first round goes out right away
}

void ModbusConfigCardWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = isDarkTheme();
    painter.setPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 18));
    painter.setBrush(dark ? QColor(255, 255, 255, 13) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

} // namespace fhmi
