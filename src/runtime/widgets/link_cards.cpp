#include "runtime/widgets/link_cards.h"

#include "comm/modbus_slave.h"
#include "comm/mqtt_link.h"
#include "comm/serial_link.h"
#include "comm/tcp_link.h"
#include "comm/udp_link.h"
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

#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace fhmi {

namespace {

const QString kProjectParser = QStringLiteral("项目设置"); // i18n: value

// A combo box of a property's options, .uidp values shown translated.
FluentQt::ComboBox *comboOf(QWidget *parent, const QStringList &values)
{
    auto *combo = new FluentQt::ComboBox(parent);
    addValueItems(combo, values);
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
    fluentToolTip(m_status);
    // A long status (a link that parses Modbus adds the poller's) takes only the room the title leaves.
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header->addWidget(m_title);
    header->addWidget(m_status, 1);
    // A single-row strip has no header: this one sits at its right end, after the button, and says the same.
    // Left-aligned, so a status too long for the strip loses the end of the poller's part, never the start.
    m_stripStatus = new FluentQt::CaptionLabel(this);
    fluentToolTip(m_stripStatus);
    m_stripStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_stripStatus->hide();
    m_grid = new QGridLayout;
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(6);
    m_grid->setColumnStretch(1, 1);
    m_parser = comboOf(this, enumOptions(meta, "defaultParser"));
    m_connect = new FluentQt::PrimaryPushButton(this);
    // Text only, as wide as the widest of its texts, so the row does not move when it changes.
    int width = 0;
    for (const QString &text : {tr("连接"), tr("断开"), tr("取消")}) {
        m_connect->setText(text);
        width = std::max(width, m_connect->sizeHint().width());
    }
    m_connect->setFixedWidth(width);
    layout->addLayout(header);
    layout->addLayout(m_grid);
    layout->addStretch();
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

void LinkCardBase::onAttached()
{
    if (text("modbusRole") == ModbusRole::Slave) {
        // The table lives while the project runs, through every connection.
        m_slave = new ModbusSlave(ModbusSlave::settingsFromProperties(properties()),
                                  ModbusSlaveStore::fromProperties(properties()), this);
        context()->hub()->addModbusSlave(m_slave);
        connect(m_slave, &ModbusSlave::statusChanged, this, [this]() { showStatus(); });
    }
    live(connect(context()->modbusPoller(), &ModbusPoller::statusChanged, this, [this]() { showStatus(); }));
    updatePolling();
}

void LinkCardBase::onDetached()
{
    context()->modbusPoller()->removeLinkCard(this);
    delete m_slave; // its links are gone already
    m_slave = nullptr;
    setConnectedUi(false, tr("未连接"));
}

ModbusPoller::Settings LinkCardBase::pollingSettings() const
{
    ModbusPoller::Settings settings;
    settings.defaultPeriod = std::clamp(integer("modbusPeriod"), 10, 60000);
    settings.timeoutMs = std::clamp(integer("modbusTimeout"), 10, 60000);
    settings.fallbackSlave = std::clamp(integer("modbusSlave"), 1, 247);
    return settings;
}

void LinkCardBase::updatePolling()
{
    if (isLive()) {
        context()->modbusPoller()->setLinkCard(this, currentLink(), pollingSettings());
        showStatus();
    }
}

bool LinkCardBase::configureParser(Link *link)
{
    CommHub *hub = context()->hub();
    QString type = selectedValue(m_parser);
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
    // A slave's counts start again with each connection.
    link->setModbusSlave(m_slave);
    if (m_slave) {
        m_slave->resetCounts();
    }
    QString error;
    if (!link->setParser(type, config, &error)) {
        context()->notify(tr("解析模式无效"), error, NotifyType::Error, 3000);
        return false;
    }
    return true;
}

void LinkCardBase::setConnectedUi(bool connected, const QString &status, const QString &toolTip)
{
    m_connected = connected;
    m_connection = status;
    m_connectionTip = toolTip;
    m_connect->setText(connected ? tr("断开") : tr("连接"));
    // The poller's status keeps the colour of a connection: its text says when a block fails.
    for (FluentQt::CaptionLabel *label : {m_status, m_stripStatus}) {
        label->setTextColor(connected ? QColor(0x10, 0x7C, 0x10) : QColor(0x80, 0x80, 0x80),
                            connected ? QColor(0x6C, 0xCB, 0x5F) : QColor(0xA0, 0xA0, 0xA0));
    }
    showStatus();
}

void LinkCardBase::showStatus()
{
    QString text = m_connection;
    QString toolTip;
    bool polling = false;
    // Never live while constructing, so currentLink() is only called on a whole card. A link that cannot
    // send (a listening server without a device) is not polled, so it shows none of the poller's status.
    if (isLive() && m_connected) {
        const Link *link = currentLink();
        const ModbusPoller *poller = context()->modbusPoller();
        if (link && link->isConnected() && link->answersModbus() && link->modbusSlave()) {
            text += QStringLiteral(" · ") + link->modbusSlave()->status();
            polling = true;
        } else if (link && link->isConnected() && link->parsesModbus() && link->canSend() &&
                   !poller->status().isEmpty() && !poller->isWaiting()) {
            text += QStringLiteral(" · ") + poller->status();
            toolTip = poller->blocks().isEmpty() ? text : poller->toolTip();
            polling = true;
        }
    }
    for (FluentQt::CaptionLabel *label : {m_status, m_stripStatus}) {
        label->setText(text);
        label->setToolTip(toolTip.isEmpty() ? m_connectionTip : toolTip);
    }
    // In the strip the poller's part may shrink away, the connection never; otherwise as wide as its text.
    m_stripStatus->setMinimumWidth(polling ? m_stripStatus->fontMetrics().horizontalAdvance(m_connection) : 0);
}

void LinkCardBase::addField(const QString &label, QWidget *field, bool small)
{
    const int row = m_grid->rowCount();
    QLabel *caption = small ? static_cast<QLabel *>(new FluentQt::CaptionLabel(label, this))
                            : static_cast<QLabel *>(new FluentQt::BodyLabel(label, this));
    m_grid->addWidget(caption, row, 0);
    m_grid->addWidget(field, row, 1);
    caption->show();
    field->show();
    m_fieldLabels.append(caption);
    m_lastField = field;
    m_lastRow = row;
}

void LinkCardBase::clearFields()
{
    const auto release = [this](QLayoutItem *item) {
        if (QWidget *widget = item->widget(); widget && widget != m_connect) {
            widget->hide();
        }
    };
    while (QLayoutItem *item = m_grid->takeAt(0)) {
        if (QLayout *row = item->layout()) { // the last row: its field and the button
            while (QLayoutItem *inner = row->takeAt(0)) {
                release(inner);
                delete inner;
            }
        } else {
            release(item);
        }
        delete item;
    }
    qDeleteAll(m_fieldLabels);
    m_fieldLabels.clear();
    m_lastField = nullptr;
    m_lastRow = -1;
}

void LinkCardBase::finishFields()
{
    if (m_lastField) {
        m_grid->removeWidget(m_lastField);
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(m_grid->horizontalSpacing());
        row->addWidget(m_lastField, 1);
        row->addWidget(m_connect);
        m_grid->addLayout(row, m_lastRow, 1);
    }
    m_connect->show();
    updateMinimumHeight();
}

void LinkCardBase::finishStrip(int column)
{
    m_grid->addWidget(m_connect, 0, column);
    m_connect->show();
    updateMinimumHeight();
}

void LinkCardBase::updateMinimumHeight()
{
    layout()->invalidate();
    layout()->activate(); // measures every row afresh
    setMinimumHeight(minimumSizeHint().height());
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
    fluentToolTip(m_refresh, tr("搜索串口"));
    keepSquareBeside(m_refresh, m_port);
    portLayout->addWidget(m_port, 1);
    portLayout->addWidget(m_refresh);
    m_baud = comboOf(this, enumOptions(meta, "defaultBaud"));
    // Data bits, parity and stop bits share one row, as in PyQt UI Designer.
    m_paramsRow = new QWidget(this);
    auto *paramsLayout = new QHBoxLayout(m_paramsRow);
    paramsLayout->setContentsMargins(0, 0, 0, 0);
    paramsLayout->setSpacing(6);
    m_dataBits = comboOf(m_paramsRow, enumOptions(meta, "defaultDatabits"));
    m_parity = comboOf(m_paramsRow, enumOptions(meta, "defaultParity"));
    m_stopBits = comboOf(m_paramsRow, enumOptions(meta, "defaultStopbits"));
    paramsLayout->addWidget(m_dataBits, 1);
    paramsLayout->addWidget(new FluentQt::CaptionLabel(tr("校验"), m_paramsRow));
    paramsLayout->addWidget(m_parity, 1);
    paramsLayout->addWidget(new FluentQt::CaptionLabel(tr("停止位"), m_paramsRow));
    paramsLayout->addWidget(m_stopBits, 1);
    m_paramsRow->hide();
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
    selectValue(m_baud, text("defaultBaud"));
    selectValue(m_dataBits, text("defaultDatabits"));
    selectValue(m_parity, text("defaultParity"));
    selectValue(m_stopBits, text("defaultStopbits"));
    selectValue(m_flow, text("defaultFlow"));
    selectValue(m_parser, text("defaultParser"));
    updatePolling(); // modbusPeriod and the like apply at once

    clearFields();
    for (QWidget *w : {m_paramsRow, static_cast<QWidget *>(m_flow), static_cast<QWidget *>(m_parser)}) {
        w->hide();
    }
    const QString mode = text("displayMode");
    const bool singleRow = mode == QStringLiteral("单行"); // i18n: value
    m_title->setVisible(!singleRow);
    m_status->setVisible(!singleRow);
    auto *layout = static_cast<QVBoxLayout *>(this->layout());
    // In a strip the port takes the width (long names like /dev/cu.usbserial-…), the baud rate a
    // fixed 90 px, as in PyQt UI Designer; in the rows every field stretches.
    m_grid->setColumnStretch(0, singleRow ? 1 : 0);
    m_grid->setColumnStretch(1, singleRow ? 0 : 1);
    if (singleRow) {
        // Port, baud rate and the button on one line, for a toolbar-like strip.
        layout->setContentsMargins(8, 4, 8, 4);
        m_baud->setFixedWidth(90);
        m_grid->addWidget(m_portRow, 0, 0);
        m_grid->addWidget(m_baud, 0, 1);
        m_grid->addWidget(m_stripStatus, 0, 3);
        m_portRow->show();
        m_baud->show();
        m_stripStatus->show();
        finishStrip(2);
        return;
    }
    layout->setContentsMargins(14, 10, 14, 12);
    m_baud->setMinimumWidth(0);
    m_baud->setMaximumWidth(QWIDGETSIZE_MAX);
    addField(tr("串口"), m_portRow);
    addField(tr("波特率"), m_baud);
    if (mode != QStringLiteral("简洁")) { // i18n: value
        addField(tr("数据位"), m_paramsRow, true);
        addField(tr("流控制"), m_flow);
        addField(tr("解析模式"), m_parser);
    }
    finishFields();
}

void SerialConfigCardWidget::refreshPorts()
{
    const QString current = m_port->currentText();
    m_port->clear();
    m_port->addItems(SerialLink::availablePorts());
    const int index = m_port->findText(current);
    if (index >= 0) {
        m_port->setCurrentIndex(index);
    }
}

void SerialConfigCardWidget::onAttached()
{
    m_link = new SerialLink(this);
    context()->hub()->addLink(m_link);
    refreshPorts();
    live(connect(m_connect, &QPushButton::clicked, this, &SerialConfigCardWidget::toggleConnection));
    live(connect(m_link, &Link::connectedChanged, this, [this](bool connected) {
        setConnectedUi(connected, connected ? m_link->description() : tr("未连接"));
        setFieldsEnabled(!connected);
    }));
    live(connect(m_link, &Link::errorOccurred, this, [this](const QString &message) {
        context()->notify(tr("串口错误"), message, NotifyType::Error, 3000);
    }));
    LinkCardBase::onAttached();
}

Link *SerialConfigCardWidget::currentLink() const
{
    return m_link;
}

void SerialConfigCardWidget::setFieldsEnabled(bool enabled)
{
    for (QWidget *w : {static_cast<QWidget *>(m_port), static_cast<QWidget *>(m_refresh),
                       static_cast<QWidget *>(m_baud), static_cast<QWidget *>(m_dataBits),
                       static_cast<QWidget *>(m_parity), static_cast<QWidget *>(m_stopBits),
                       static_cast<QWidget *>(m_flow), static_cast<QWidget *>(m_parser)}) {
        w->setEnabled(enabled);
    }
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
    setFieldsEnabled(true);
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
    settings.baudRate = selectedValue(m_baud).toInt();
    settings.dataBits = selectedValue(m_dataBits);
    settings.parity = selectedValue(m_parity);
    settings.stopBits = selectedValue(m_stopBits);
    settings.flowControl = selectedValue(m_flow);
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
    selectValue(m_mode, text("defaultMode"));
    m_host->setText(text("defaultHost"));
    m_port->setValue(integer("defaultPort"));
    selectValue(m_parser, text("defaultParser"));
    updatePolling();
    clearFields();
    m_parser->hide();
    m_mode->hide();
    addField(tr("模式"), m_mode);
    addField(tr("地址"), m_host);
    addField(tr("端口"), m_port);
    if (text("displayMode") != QStringLiteral("简洁")) { // i18n: value
        addField(tr("解析模式"), m_parser);
    }
    finishFields();
}

void TcpConfigCardWidget::onAttached()
{
    live(connect(m_connect, &QPushButton::clicked, this, &TcpConfigCardWidget::toggleConnection));
    LinkCardBase::onAttached();
}

Link *TcpConfigCardWidget::currentLink() const
{
    return m_link;
}

void TcpConfigCardWidget::onDetached()
{
    dropLink();
    LinkCardBase::onDetached();
    setFieldsEnabled(true);
}

void TcpConfigCardWidget::setFieldsEnabled(bool enabled)
{
    for (QWidget *w : {static_cast<QWidget *>(m_mode), static_cast<QWidget *>(m_host), static_cast<QWidget *>(m_port),
                       static_cast<QWidget *>(m_parser)}) {
        w->setEnabled(enabled);
    }
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
    updatePolling();
}

void TcpConfigCardWidget::toggleConnection()
{
    if (m_link) {
        // Disconnect, or cancel a pending connection attempt.
        dropLink();
        setConnectedUi(false, tr("未连接"));
        setFieldsEnabled(true);
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
    updatePolling();
    connect(link, &Link::connectedChanged, this, [this, link](bool connected) {
        setConnectedUi(connected, connected ? link->description() : tr("未连接"));
        if (!connected && m_link == link && qobject_cast<TcpClientLink *>(link)) {
            // A client that lost its device is done; the next click connects afresh.
            QTimer::singleShot(0, this, [this, link]() {
                if (m_link == link) {
                    dropLink();
                    setFieldsEnabled(true);
                }
            });
        }
    });
    connect(link, &Link::errorOccurred, this, [this](const QString &message) {
        context()->notify(tr("TCP 错误"), message, NotifyType::Error, 3000);
    });
    setFieldsEnabled(false);
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
            setFieldsEnabled(true);
        }
    } else {
        setConnectedUi(false, tr("连接中..."));
        m_connect->setText(tr("取消"));
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
    m_password = new FluentQt::PasswordLineEdit(this);
    m_password->setPlaceholderText(tr("可选"));
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
    selectValue(m_qos, text("defaultQos"));
    m_tls->setChecked(flag("defaultTls"));
    selectValue(m_parser, text("defaultParser"));
    updatePolling();

    clearFields();
    for (QWidget *w : {static_cast<QWidget *>(m_clientId), m_loginRow, m_qosRow, static_cast<QWidget *>(m_parser)}) {
        w->hide();
    }
    const bool full = text("displayMode") != QStringLiteral("简洁"); // i18n: value
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
    finishFields();
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
    LinkCardBase::onAttached();
}

Link *MqttConfigCardWidget::currentLink() const
{
    return m_link;
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
    settings.qos = selectedValue(m_qos).toInt();
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
// UdpConfigCard
// ---------------------------------------------------------------------------

UdpConfigCardWidget::UdpConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : LinkCardBase(meta, parent)
{
    m_localPort = new FluentQt::SpinBox(this);
    m_localPort->setRange(0, 65535);
    m_localPort->setSpecialValueText(tr("自动"));
    m_localPort->hide();
    m_host = new FluentQt::LineEdit(this);
    m_host->setPlaceholderText(tr("留空则回复最后发送方"));
    m_port = new FluentQt::SpinBox(this);
    m_port->setRange(1, 65535);
    // Host and port share a row, as the MQTT card's server.
    m_remoteRow = new QWidget(this);
    auto *remoteLayout = new QHBoxLayout(m_remoteRow);
    remoteLayout->setContentsMargins(0, 0, 0, 0);
    remoteLayout->setSpacing(6);
    m_host->setParent(m_remoteRow);
    m_port->setParent(m_remoteRow);
    remoteLayout->addWidget(m_host, 3);
    remoteLayout->addWidget(m_port, 1);
    m_remoteRow->hide();
}

UdpConfigCardWidget::~UdpConfigCardWidget()
{
    detach();
}

void UdpConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    m_localPort->setValue(integer("defaultLocalPort"));
    m_host->setText(text("defaultHost"));
    m_port->setValue(integer("defaultPort"));
    selectValue(m_parser, text("defaultParser"));
    updatePolling();
    clearFields();
    m_parser->hide();
    addField(tr("本地端口"), m_localPort);
    addField(tr("远程"), m_remoteRow);
    if (text("displayMode") != QStringLiteral("简洁")) { // i18n: value
        addField(tr("解析模式"), m_parser);
    }
    finishFields();
}

void UdpConfigCardWidget::onAttached()
{
    m_link = new UdpLink(this);
    context()->hub()->addLink(m_link);
    live(connect(m_connect, &QPushButton::clicked, this, &UdpConfigCardWidget::toggleConnection));
    live(connect(m_link, &Link::connectedChanged, this, &UdpConfigCardWidget::refreshState));
    // The first datagram of a link without a remote gives it someone to send to: the poller may start.
    live(connect(m_link, &Link::canSendChanged, this, &UdpConfigCardWidget::refreshState));
    live(connect(m_link, &Link::errorOccurred, this, [this](const QString &message) {
        context()->notify(QStringLiteral("UDP"), message, NotifyType::Error, 3000);
    }));
    LinkCardBase::onAttached();
}

Link *UdpConfigCardWidget::currentLink() const
{
    return m_link;
}

void UdpConfigCardWidget::onDetached()
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

void UdpConfigCardWidget::toggleConnection()
{
    if (m_link->isConnected()) {
        m_link->close();
        return;
    }
    if (!configureParser(m_link)) {
        return;
    }
    QString error;
    if (!m_link->open(quint16(m_localPort->value()), m_host->text(), m_port->value(), &error)) {
        context()->notify(QStringLiteral("UDP"), error, NotifyType::Error, 3000);
    }
}

void UdpConfigCardWidget::refreshState()
{
    const bool open = m_link && m_link->isConnected();
    for (QWidget *w : {static_cast<QWidget *>(m_localPort), static_cast<QWidget *>(m_host),
                       static_cast<QWidget *>(m_port), static_cast<QWidget *>(m_parser)}) {
        w->setEnabled(!open);
    }
    // Like the other cards here, the status is the link's description.
    setConnectedUi(open, open ? m_link->description() : tr("未连接"));
}

} // namespace fhmi
