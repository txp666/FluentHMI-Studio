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

#include <algorithm>

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

const QString kTableMode = QStringLiteral("轮询表");

// A block counts as due this much early, so a tick that arrives a moment before a block's time does
// not leave it waiting a whole further tick.
constexpr qint64 kDueSlackMs = 10;

// Why a poll-table read failed, as its round reports it: 无响应, 异常 02 非法数据地址, 发送失败.
QString roundProblem(const ModbusResult &result)
{
    if (result.ok) {
        return {};
    }
    if (result.timedOut) {
        return QStringLiteral("无响应");
    }
    const QString exception = modbusException(result.message);
    return exception.isEmpty() ? result.problem : exception;
}

} // namespace

ModbusConfigCardWidget::ModbusConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    // The layout of PyQt UI Designer's card: plain number fields fit its 380 x 180 default,
    // where spin boxes would leave no room for the digits.
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 10, 16, 10);
    layout->setSpacing(6);
    auto *header = new QGridLayout;
    header->setHorizontalSpacing(8);
    header->setVerticalSpacing(0);
    m_title = new FluentQt::StrongBodyLabel(this);
    m_modeHint = new FluentQt::CaptionLabel(this); // 轮询方式：自动, in small type under the title
    m_modeHint->setTextColor(QColor(0x80, 0x80, 0x80), QColor(0xa0, 0xa0, 0xa0));
    m_status = new FluentQt::CaptionLabel(tr("就绪"), this);
    m_status->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xb0, 0xb0, 0xb0));
    // A long status takes only the room the title leaves; the tooltip has all of it.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(m_title, 0, 0);
    header->addWidget(m_status, 0, 1);
    header->addWidget(m_modeHint, 1, 0, 1, 2);
    header->setColumnStretch(1, 1);

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
    m_modeHint->setText(autoMode() ? tr("轮询方式：自动") : tr("轮询方式：轮询表"));
    m_slave->setText(QString::number(integer("defaultSlave")));
    selectText(m_function, text("defaultFunc"));
    m_address->setText(QString::number(integer("defaultAddr")));
    m_value->setText(text("defaultValue")); // older projects hold an int; text() reads both
    m_poll->setText(QString::number(integer("defaultPoll")));
    updateValueLabel();
}

bool ModbusConfigCardWidget::autoMode() const
{
    return text("pollMode") != kTableMode;
}

void ModbusConfigCardWidget::onAttached()
{
    CommHub *hub = context()->hub();
    live(connect(m_send, &QPushButton::clicked, this, &ModbusConfigCardWidget::sendFields));
    live(connect(m_pollButton, &QPushButton::clicked, this, &ModbusConfigCardWidget::togglePolling));
    live(connect(m_timer, &QTimer::timeout, this, &ModbusConfigCardWidget::pollTick));
    live(connect(hub, &CommHub::linkConnected, this,
                 [this](const QString &link) { setStatus(tr("已连接: %1").arg(link)); }));
    live(connect(hub, &CommHub::linkUp, this, &ModbusConfigCardWidget::maybeAutoStart));
    live(connect(hub, &CommHub::connectionStateChanged, this, [this](bool any) {
        if (!any) {
            stopPolling();
            setStatus(tr("未连接"));
        }
    }));
    // A link that is up before the surface starts counts as just connected.
    for (Link *link : hub->connectedLinks()) {
        maybeAutoStart(link);
    }
}

void ModbusConfigCardWidget::onDetached()
{
    stopPolling();
    m_manual.reset();
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

int ModbusConfigCardWidget::defaultPeriod() const
{
    const int ms = m_poll->text().toInt();
    return ms > 0 ? ms : 500;
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
        if (link->parsesModbus()) {
            return true;
        }
    }
    return false;
}

QList<Link *> ModbusConfigCardWidget::modbusLinks() const
{
    QList<Link *> links;
    for (Link *link : context()->hub()->connectedLinks()) {
        if (link->parsesModbus() && link->canSend()) {
            links.append(link);
        }
    }
    return links;
}

// ----- requests and their replies -----

bool ModbusConfigCardWidget::queue(const ModbusRequest &request, ModbusMaster::Priority priority,
                                   std::optional<Pending> &pending, std::function<void(const ModbusResult &)> complete,
                                   bool announce, bool modbusLinksOnly)
{
    const int token = ++m_token;
    pending = Pending{token, std::move(complete), {}, {}, false, std::nullopt};
    QList<QPointer<ModbusMaster>> masters;
    ModbusSend how;
    how.timeoutMs = std::max(10, integer("timeout"));
    how.priority = priority;
    how.owner = &pending;
    how.modbusLinksOnly = modbusLinksOnly;
    how.queuedOn = &masters;
    std::optional<Pending> *slot = &pending;
    how.done = [self = QPointer<ModbusConfigCardWidget>(this), slot, token](const ModbusResult &result) {
        if (self) {
            self->report(*slot, token, result);
        }
    };
    m_lastSent.clear();
    if (!context()->sendModbus(request, &m_lastSent, how)) {
        pending.reset();
        return false;
    }
    if (announce) {
        setStatus(sentStatus());
    }
    if (!pending || pending->token != token) {
        return true;
    }
    if (masters.isEmpty()) {
        pending.reset(); // only links that do not parse Modbus took it: no reply to wait for
        return true;
    }
    for (const QPointer<ModbusMaster> &master : std::as_const(masters)) {
        if (!pending->reported.contains(master.data())) {
            pending->waiting.append(master);
        }
    }
    pending->armed = true;
    finishIfSettled(pending);
    return true;
}

void ModbusConfigCardWidget::report(std::optional<Pending> &pending, int token, const ModbusResult &result)
{
    if (!pending || pending->token != token) {
        return; // stopped, or a request since took its place
    }
    if (!pending->result || (result.ok && !pending->result->ok)) {
        pending->result = result;
    }
    if (!pending->armed) {
        pending->reported.append(result.master.data());
        return;
    }
    pending->waiting.removeAll(result.master);
    finishIfSettled(pending);
}

void ModbusConfigCardWidget::finishIfSettled(std::optional<Pending> &pending)
{
    if (!pending || !pending->armed) {
        return;
    }
    pending->waiting.removeIf([](const QPointer<ModbusMaster> &master) { return master.isNull(); });
    if (!pending->waiting.isEmpty()) {
        return;
    }
    Pending done = std::move(*pending);
    pending.reset();
    ModbusResult result;
    if (done.result) {
        result = *done.result;
    } else { // every link it went to is gone
        result.timedOut = true;
        result.cancelled = true;
    }
    if (done.complete) {
        done.complete(result);
    }
}

void ModbusConfigCardWidget::sendFields()
{
    const auto request = fieldRequest();
    if (!request) {
        return;
    }
    // "已发送" only means the links took it; the reply, or its absence, follows.
    if (!queue(*request, ModbusMaster::Priority::Write, m_manual, [this](const ModbusResult &r) { manualDone(r); },
               true)) {
        stopPolling();
        setStatus(tr("发送失败"));
    }
}

void ModbusConfigCardWidget::manualDone(const ModbusResult &result)
{
    if (m_timer->isActive()) {
        return; // polling has the status line now
    }
    setStatus(result.ok ? tr("设备已响应") : result.problem);
}

// ----- polling -----

void ModbusConfigCardWidget::togglePolling()
{
    if (m_timer->isActive()) {
        stopPolling();
    } else {
        startPolling(false);
    }
}

void ModbusConfigCardWidget::maybeAutoStart(Link *link)
{
    if (!flag("autoStart") || m_timer->isActive() || !link || !link->parsesModbus() || !link->canSend()) {
        return;
    }
    startPolling(true);
}

void ModbusConfigCardWidget::startPolling(bool automatic)
{
    if (autoMode()) {
        m_autoPolling = true;
        m_read.reset();
        m_blocks.clear();
        m_due.clear();
        m_blockResults.clear();
        ++m_plan;
        m_timer->setTimerType(Qt::PreciseTimer); // a block's period is kept to the tick
        m_timer->start(defaultPeriod());
        m_pollButton->setText(tr("停止"));
        m_send->setEnabled(false);
        schedule();
        return;
    }
    const int interval = m_poll->text().toInt();
    if (interval <= 0) {
        if (!automatic) {
            context()->notify(QStringLiteral("Modbus"), tr("请先设置轮询间隔"), NotifyType::Warning);
        }
        return;
    }
    QString error;
    const auto table = CommandEncoder::modbusPollTable(text("pollList"), &error);
    if (!table) {
        context()->notify(QStringLiteral("Modbus"), error, NotifyType::Error);
        setStatus(error);
        return;
    }
    m_autoPolling = false;
    m_table = *table;
    m_read.reset();
    m_round.clear();
    m_next = 0;
    m_timer->setTimerType(Qt::CoarseTimer);
    m_timer->start(interval);
    m_pollButton->setText(tr("停止"));
    m_send->setEnabled(false); // a request of its own would take the place of the one polling waits for
    if (!m_table.isEmpty()) {
        setStatus(tr("轮询中 · %1 条").arg(m_table.size()));
    }
    pollTick(); // the first round goes out right away
}

void ModbusConfigCardWidget::stopPolling()
{
    m_timer->stop();
    if (m_read) { // its reads still waiting in a queue never go out
        for (const QPointer<ModbusMaster> &master : std::as_const(m_read->waiting)) {
            if (master) {
                master->cancelQueued(&m_read);
            }
        }
    }
    m_read.reset();
    m_round.clear();
    m_autoPolling = false;
    m_pollButton->setText(tr("轮询"));
    m_send->setEnabled(true);
}

void ModbusConfigCardWidget::pollTick()
{
    if (m_autoPolling) {
        schedule();
    } else {
        tableTick();
    }
}

// ----- 轮询表 -----

void ModbusConfigCardWidget::tableTick()
{
    finishIfSettled(m_read);
    if (m_read) {
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
        if (request && !queue(*request, ModbusMaster::Priority::Read, m_read, {}, true)) {
            stopPolling(); // a failed write stops polling
            setStatus(tr("发送失败"));
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
    sendNextInRound();
}

void ModbusConfigCardWidget::sendNextInRound()
{
    const int item = m_roundSize - int(m_round.size()) + 1;
    const ModbusRequest request = m_round.takeFirst();
    if (!queue(request, ModbusMaster::Priority::Read, m_read,
               [this, item](const ModbusResult &result) { tableDone(item, result); }, false)) {
        stopPolling();
        setStatus(tr("发送失败"));
    }
}

void ModbusConfigCardWidget::tableDone(int item, const ModbusResult &result)
{
    if (!m_timer->isActive() || m_autoPolling || result.cancelled) {
        return; // a link that went down: the next tick starts a new round
    }
    if (const QString problem = roundProblem(result); !problem.isEmpty()) {
        m_roundProblems.append({item, problem});
    }
    if (!m_round.isEmpty()) {
        sendNextInRound();
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

// ----- 自动 -----

void ModbusConfigCardWidget::schedule()
{
    if (!m_autoPolling || !isLive()) {
        return;
    }
    finishIfSettled(m_read);
    const QList<Link *> links = modbusLinks();
    if (links.isEmpty()) {
        if (!m_blocks.isEmpty()) { // a link that comes back starts with a fresh plan
            m_blocks.clear();
            m_due.clear();
            m_blockResults.clear();
            ++m_plan;
        }
        setAutoStatus(tr("自动 · 等待连接"));
        return;
    }
    replan();
    if (m_blocks.isEmpty() || m_read) {
        return;
    }
    for (Link *link : links) {
        if (link->modbusMaster()->busy()) {
            return;
        }
    }
    const qint64 now = context()->now();
    int next = -1;
    for (int i = 0; i < int(m_blocks.size()); ++i) {
        if (m_due.at(i) <= now + kDueSlackMs && (next < 0 || m_due.at(i) < m_due.at(next))) {
            next = i;
        }
    }
    if (next < 0) {
        return;
    }
    const ModbusBlock block = m_blocks.at(next);
    m_due[next] = now + block.period;
    const auto request = CommandEncoder::modbusRequest(block.slave, block.fc, block.start, block.count);
    if (!request) {
        return; // not reached: the planner keeps blocks within what a request can read
    }
    const int plan = m_plan;
    if (!queue(*request, ModbusMaster::Priority::Read, m_read,
               [this, next, plan](const ModbusResult &result) { blockDone(next, plan, result); }, false, true)) {
        stopPolling();
        setStatus(tr("发送失败"));
    }
}

void ModbusConfigCardWidget::replan()
{
    QList<ModbusNeed> active;
    const int fallbackSlave = integer("defaultSlave");
    for (ModbusNeed need : context()->modbusNeeds()) {
        if (!need.visible) {
            continue; // on a tab page in the background, or hidden
        }
        if (need.slave <= 0) {
            need.slave = fallbackSlave;
        }
        if (need.slave > 247) {
            continue; // no request can address it
        }
        need.period = std::max(0, need.period);
        active.append(need);
    }
    const QList<ModbusBlock> blocks = planBlocks(active, defaultPeriod());
    if (blocks == m_blocks) {
        if (blocks.isEmpty()) {
            setAutoStatus(tr("自动 · 没有 Modbus 绑定"));
        }
        return;
    }
    // A new plan starts a new round. A block that stays keeps its time; a new one is read right away.
    QList<qint64> due(blocks.size(), 0);
    for (int i = 0; i < int(blocks.size()); ++i) {
        const qsizetype old = m_blocks.indexOf(blocks.at(i));
        if (old >= 0) {
            due[i] = m_due.at(old);
        }
    }
    m_blocks = blocks;
    m_due = due;
    m_blockResults = QList<std::optional<ModbusResult>>(blocks.size());
    ++m_plan;
    int interval = defaultPeriod();
    for (const ModbusBlock &block : blocks) {
        interval = std::min(interval, block.period);
    }
    interval = std::max(1, interval);
    if (m_timer->interval() != interval) {
        m_timer->setInterval(interval);
    }
    setAutoStatus(blocks.isEmpty() ? tr("自动 · 没有 Modbus 绑定") : tr("自动 · %1 块 · 轮询中").arg(blocks.size()));
}

void ModbusConfigCardWidget::blockDone(int block, int plan, const ModbusResult &result)
{
    if (!m_autoPolling || result.cancelled) {
        return;
    }
    if (plan == m_plan && block < int(m_blockResults.size())) {
        m_blockResults[block] = result;
        if (std::all_of(m_blockResults.cbegin(), m_blockResults.cend(),
                        [](const std::optional<ModbusResult> &r) { return r.has_value(); })) {
            roundDone();
        }
    }
    schedule(); // the next due block goes out right away
}

void ModbusConfigCardWidget::roundDone()
{
    const int count = int(m_blocks.size());
    QString status = tr("自动 · %1 块 · 正常").arg(count);
    for (int k = 0; k < count; ++k) {
        const ModbusResult &result = *m_blockResults.at(k);
        if (result.ok) {
            continue;
        }
        const ModbusBlock &block = m_blocks.at(k);
        const QString exception = modbusException(result.message);
        status = exception.isEmpty() ? tr("自动 · 第 %1 块无响应（从站 %2 功能码 %3 地址 %4）")
                                           .arg(k + 1)
                                           .arg(block.slave)
                                           .arg(block.fc)
                                           .arg(block.start)
                                     : tr("自动 · 第 %1 块%2").arg(k + 1).arg(exception);
        break;
    }
    m_blockResults = QList<std::optional<ModbusResult>>(count);
    setAutoStatus(status);
}

void ModbusConfigCardWidget::setAutoStatus(const QString &text)
{
    m_status->setText(text);
    QStringList lines;
    for (const ModbusBlock &block : std::as_const(m_blocks)) {
        lines << tr("从站 %1 · 功能码 %2 · 地址 %3–%4 · 周期 %5 ms")
                     .arg(block.slave)
                     .arg(block.fc)
                     .arg(block.start)
                     .arg(block.start + block.count - 1)
                     .arg(block.period);
    }
    m_status->setToolTip(lines.isEmpty() ? text : lines.join(QLatin1Char('\n')));
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
