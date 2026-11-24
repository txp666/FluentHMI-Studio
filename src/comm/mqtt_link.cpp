#include "comm/mqtt_link.h"

#include <QtCore/QUuid>

#include <algorithm>

namespace fhmi {

namespace {

constexpr qsizetype PublishWindow = 8;
constexpr qint64 PublishBufferLimit = 256 * 1024;
constexpr int kMaxReconnectDelayMs = 30000;
constexpr int kHandshakeTimeoutMs = 15000;
constexpr int kAckCheckIntervalMs = 2000;
constexpr qint64 kAckTimeoutMs = 10000;
constexpr int kMaxAckRetries = 2;
constexpr qsizetype kMaxMqttUtf8Bytes = 65535;
// Wildcard subscriptions can fan out widely; parsers beyond this are dropped and rebuilt.
constexpr int kMaxTopicParsers = 256;

} // namespace

MqttLink::MqttLink(QObject *parent) : Link(parent)
{
    m_clock.start();
    m_handshakeTimer.setSingleShot(true);
    m_ackTimer.setSingleShot(false);
    m_ackTimer.setInterval(kAckCheckIntervalMs);
    m_pingTimer.setSingleShot(false);
    m_reconnectTimer.setSingleShot(true);
    connect(&m_socket, &QSslSocket::connected, this, [this]() {
        if (!m_settings.tls) {
            sendConnect();
        }
    });
    connect(&m_socket, &QSslSocket::encrypted, this, &MqttLink::sendConnect);
    connect(&m_socket, &QSslSocket::readyRead, this, &MqttLink::onReadyRead);
    connect(&m_socket, &QSslSocket::errorOccurred, this, &MqttLink::onSocketError);
    connect(&m_socket, &QSslSocket::disconnected, this, &MqttLink::onSocketDisconnected);
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this]() {
        if (!m_wanted || m_up) {
            return;
        }
        m_socket.blockSignals(true);
        m_socket.abort();
        m_socket.blockSignals(false);
        if (m_everUp) {
            scheduleReconnect();
        } else {
            giveUp(tr("MQTT 连接超时"));
        }
    });
    connect(&m_ackTimer, &QTimer::timeout, this, &MqttLink::checkOutboundAcks);
    connect(&m_pingTimer, &QTimer::timeout, this, [this]() {
        // No packet for one and a half keep-alive periods: the broker is gone.
        if (m_sinceReceived.isValid() && m_sinceReceived.elapsed() > m_settings.keepAlive * 1500) {
            connectionLost(tr("心跳超时"));
            return;
        }
        sendPacket(Mqtt::encodePingReq());
    });
    connect(&m_reconnectTimer, &QTimer::timeout, this, &MqttLink::openSocket);
}

MqttLink::~MqttLink()
{
    m_socket.blockSignals(true);
    m_socket.abort();
}

bool MqttLink::connectTo(const MqttSettings &requested, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = tr("MQTT 参数无效: %1").arg(message);
        }
        return false;
    };
    MqttSettings settings = requested;
    settings.host = settings.host.trimmed();
    settings.publishTopic = Mqtt::expandTopicSession(settings.publishTopic.trimmed(), settings.topicSession);
    for (auto &topic : settings.topics)
        topic = Mqtt::expandTopicSession(topic, settings.topicSession);
    if (settings.host.isEmpty()) {
        return fail(tr("请填写 MQTT 服务器地址"));
    }
    if (settings.port < 1 || settings.port > 65535) {
        return fail(tr("端口必须在 1–65535 之间"));
    }
    if (settings.qos < 0 || settings.qos > 2) {
        return fail(tr("QoS 只能是 0、1 或 2"));
    }
    if (settings.tls && !QSslSocket::supportsSsl()) {
        return fail(tr("此平台没有可用的 TLS 支持"));
    }
    if (settings.clientId.trimmed().isEmpty()) {
        settings.clientId = QStringLiteral("fluenthmi-") + QUuid::createUuid().toString(QUuid::Id128).left(8);
    }
    const auto tooLong = [](const QString &value) { return value.toUtf8().size() > kMaxMqttUtf8Bytes; };
    if (tooLong(settings.clientId) || tooLong(settings.username) || tooLong(settings.password) ||
        tooLong(settings.publishTopic) || std::any_of(settings.topics.cbegin(), settings.topics.cend(), tooLong)) {
        return fail(tr("MQTT 文本字段不能超过 65535 字节"));
    }

    close();
    m_settings = settings;
    m_wanted = true;
    m_everUp = false;
    m_reconnectDelayMs = 1000;
    openSocket();
    return true;
}

void MqttLink::openSocket()
{
    if (!m_wanted) {
        return;
    }
    m_socket.abort();
    m_decoder.reset();
    m_handshakeTimer.start(kHandshakeTimeoutMs);
    if (m_settings.tls) {
        // The system's trust store decides, including certificates the user added to it.
        m_socket.connectToHostEncrypted(m_settings.host, quint16(m_settings.port));
    } else {
        m_socket.connectToHost(m_settings.host, quint16(m_settings.port));
    }
}

void MqttLink::sendConnect()
{
    Mqtt::ConnectOptions options;
    options.clientId = m_settings.clientId;
    options.username = m_settings.username;
    options.password = m_settings.password;
    options.keepAlive = m_settings.keepAlive;
    m_sinceReceived.start();
    sendPacket(Mqtt::encodeConnect(options));
}

void MqttLink::close()
{
    const bool wasActive = m_wanted || m_up;
    m_wanted = false;
    m_handshakeTimer.stop();
    m_ackTimer.stop();
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    if (m_up) {
        m_socket.write(Mqtt::encodeDisconnect());
        m_socket.flush();
    }
    m_up = false;
    m_socket.blockSignals(true);
    m_socket.abort();
    m_socket.blockSignals(false);
    m_decoder.reset();
    dropParsers();
    m_pendingSubscriptions.clear();
    m_inboundQos2.clear();
    m_outbound.clear();
    if (wasActive) {
        emit connectedChanged(false);
    }
}

QString MqttLink::description() const
{
    return QStringLiteral("MQTT %1:%2").arg(m_settings.host).arg(m_settings.port);
}

quint16 MqttLink::nextPacketId()
{
    for (int i = 0; i < 0xFFFF; ++i) {
        m_lastPacketId = m_lastPacketId == 0xFFFF ? 1 : quint16(m_lastPacketId + 1);
        if (!m_outbound.contains(m_lastPacketId) && !m_pendingSubscriptions.contains(m_lastPacketId))
            return m_lastPacketId;
    }
    return 0;
}

bool MqttLink::sendPacket(const QByteArray &packet)
{
    return m_socket.state() == QAbstractSocket::ConnectedState && m_socket.write(packet) == packet.size();
}

void MqttLink::dropParsers()
{
    for (const auto &parser : std::as_const(m_parsers)) dropStream(parser.get());
    m_parsers.clear();
}

qint64 MqttLink::writeBytes(const QByteArray &bytes)
{
    return writeTo(bytes, m_settings.publishTopic);
}

bool MqttLink::canSendTo(const QString &endpoint) const
{
    const QString &topic = endpoint.isEmpty() ? m_settings.publishTopic : endpoint;
    return m_up && !topic.isEmpty() && !topic.contains(QLatin1Char('+')) && !topic.contains(QLatin1Char('#'));
}

QStringList MqttLink::sendEndpoints() const
{
    return {Mqtt::restoreTopicSession(m_settings.publishTopic, m_settings.topicSession)};
}

bool MqttLink::hasPendingWrite(const QString &) const
{
    return m_socket.bytesToWrite() > 0 || m_outbound.size() >= PublishWindow;
}

qint64 MqttLink::writeTo(const QByteArray &bytes, const QString &endpoint)
{
    const QString topic = endpoint.isEmpty() ? m_settings.publishTopic : endpoint;
    if (!canSendTo(topic)) {
        emit errorOccurred(tr("发送失败: 未设置发布主题"), endpoint);
        return WriteFailureReported;
    }
    if (m_socket.bytesToWrite() >= PublishBufferLimit || m_outbound.size() >= PublishWindow) {
        emit errorOccurred(tr("MQTT 发送队列已满"), topic);
        return WriteFailureReported;
    }
    Mqtt::PublishMessage message;
    message.topic = Mqtt::expandTopicSession(topic, m_settings.topicSession);
    if (message.topic.toUtf8().size() > kMaxMqttUtf8Bytes) {
        emit errorOccurred(tr("MQTT 文本字段不能超过 65535 字节"), topic);
        return WriteFailureReported;
    }
    message.payload = bytes;
    message.qos = m_settings.qos;
    if (message.qos > 0) {
        message.packetId = nextPacketId();
        if (!message.packetId) {
            emit errorOccurred(tr("MQTT 发送队列已满"), topic);
            return WriteFailureReported;
        }
    }
    const QByteArray wire = Mqtt::encodePublish(message);
    if (!sendPacket(wire))
        return -1;
    if (message.packetId) {
        m_outbound.insert(message.packetId,
                          OutboundPacket{message.qos == 1 ? Mqtt::PubAck : Mqtt::PubRec, wire, m_clock.elapsed(), 0});
        if (!m_ackTimer.isActive()) {
            m_ackTimer.start();
        }
    }
    return bytes.size();
}

void MqttLink::onReadyRead()
{
    const QByteArray data = m_socket.readAll();
    m_sinceReceived.restart();
    for (const Mqtt::Packet &packet : m_decoder.feed(data)) {
        handlePacket(packet);
        if (m_socket.state() != QAbstractSocket::ConnectedState) {
            return; // a refusal closed the link
        }
    }
    if (m_decoder.hasError()) {
        connectionLost(tr("收到无效数据"));
    }
}

void MqttLink::handlePacket(const Mqtt::Packet &packet)
{
    switch (packet.type) {
    case Mqtt::ConnAck: {
        m_handshakeTimer.stop();
        const int code = packet.body.size() >= 2 ? quint8(packet.body.at(1)) : -1;
        if (code != 0) {
            giveUp(tr("MQTT 连接失败: %1").arg(Mqtt::connAckText(code)));
            return;
        }
        m_up = true;
        m_everUp = true;
        m_reconnectDelayMs = 1000;
        dropParsers();
        if (!m_settings.topics.isEmpty()) {
            const quint16 id = nextPacketId();
            if (id && sendPacket(Mqtt::encodeSubscribe(id, m_settings.topics, m_settings.qos)))
                m_pendingSubscriptions.insert(id, m_settings.topics);
        }
        if (m_settings.keepAlive > 0) {
            m_pingTimer.start(m_settings.keepAlive * 750);
        }
        emit connectedChanged(true);
        break;
    }
    case Mqtt::SubAck: {
        const auto id = Mqtt::packetId(packet);
        const QStringList topics = id ? m_pendingSubscriptions.take(*id) : QStringList();
        for (int i = 2; i < packet.body.size() && i - 2 < topics.size(); ++i) {
            if (quint8(packet.body.at(i)) == 0x80) {
                emit errorOccurred(tr("订阅 %1 失败").arg(topics.at(i - 2)));
            }
        }
        break;
    }
    case Mqtt::Publish:
        if (const auto message = Mqtt::decodePublish(packet)) {
            if (message->qos == 1) {
                sendPacket(Mqtt::encodeAck(Mqtt::PubAck, message->packetId));
                handleMessage(*message);
            } else if (message->qos == 2) {
                // Exactly once: a redelivered id before its PUBREL is the same message.
                if (!m_inboundQos2.contains(message->packetId)) {
                    m_inboundQos2.insert(message->packetId);
                    handleMessage(*message);
                }
                sendPacket(Mqtt::encodeAck(Mqtt::PubRec, message->packetId));
            } else {
                handleMessage(*message);
            }
        }
        break;
    case Mqtt::PubRel:
        if (const auto id = Mqtt::packetId(packet)) {
            m_inboundQos2.remove(*id);
            sendPacket(Mqtt::encodeAck(Mqtt::PubComp, *id));
        }
        break;
    case Mqtt::PubRec:
        if (const auto id = Mqtt::packetId(packet)) {
            auto awaited = m_outbound.find(*id);
            if (awaited != m_outbound.end() &&
                (awaited->awaiting == Mqtt::PubRec || awaited->awaiting == Mqtt::PubComp)) {
                awaited->awaiting = Mqtt::PubComp;
                awaited->wire = Mqtt::encodeAck(Mqtt::PubRel, *id);
                awaited->sentAtMs = m_clock.elapsed();
                awaited->retries = 0;
                if (!sendPacket(awaited->wire))
                    connectionLost(tr("MQTT 发布确认失败"));
            }
        }
        break;
    case Mqtt::PubAck:
    case Mqtt::PubComp:
        if (const auto id = Mqtt::packetId(packet)) {
            auto awaited = m_outbound.find(*id);
            if (awaited != m_outbound.end() && awaited->awaiting == packet.type) {
                m_outbound.erase(awaited);
                if (m_outbound.isEmpty()) {
                    m_ackTimer.stop();
                }
            }
        }
        break;
    default:
        break; // PINGRESP and anything else only prove the broker is alive
    }
}

void MqttLink::handleMessage(const Mqtt::PublishMessage &message)
{
    if (!m_up) {
        return;
    }
    const auto topic = Mqtt::restoreTopicSession(message.topic, m_settings.topicSession);
    auto it = m_parsers.find(topic);
    if (it == m_parsers.end()) {
        if (m_parsers.size() >= kMaxTopicParsers) {
            dropParsers();
        }
        it = m_parsers.insert(topic, std::shared_ptr<Parser>(createStreamParser(topic)));
        registerStream(it->get(), topic);
    }
    const std::shared_ptr<Parser> parser = *it; // a slot may reconnect and clear m_parsers
    deliver(parser.get(), message.payload, Boundary::Message);
}

void MqttLink::annotateMessage(Message &message, const DataSource &source) const
{
    if (message.size() == 1 && message.contains(QStringLiteral("ch0")))
        message.insert(source.endpoint, message.value(QStringLiteral("ch0")));
}

void MqttLink::onSocketError(QAbstractSocket::SocketError error)
{
    if (!m_wanted) {
        return;
    }
    if (m_everUp) {
        if (!m_up) {
            scheduleReconnect(); // a reconnect attempt failed; keep trying
        } else if (error != QAbstractSocket::RemoteHostClosedError) {
            connectionLost(m_socket.errorString());
        }
        return;
    }
    QString message = m_socket.errorString();
    if (m_settings.tls) {
        // A wrong port and an untrusted certificate look alike from here.
        message += tr("（已启用 TLS，请检查端口和服务器证书）");
    }
    giveUp(tr("MQTT 连接失败: %1").arg(message));
}

void MqttLink::onSocketDisconnected()
{
    if (!m_wanted) {
        return;
    }
    if (m_up) {
        connectionLost(QString());
    } else if (m_everUp) {
        scheduleReconnect();
    } else {
        giveUp(tr("MQTT 连接失败: 服务器关闭了连接"));
    }
}

void MqttLink::connectionLost(const QString &reason)
{
    if (!m_up) {
        return;
    }
    m_up = false;
    m_handshakeTimer.stop();
    m_ackTimer.stop();
    m_pingTimer.stop();
    dropParsers();
    m_pendingSubscriptions.clear();
    m_inboundQos2.clear();
    m_outbound.clear();
    m_socket.blockSignals(true);
    m_socket.abort();
    m_socket.blockSignals(false);
    emit errorOccurred(tr("MQTT 连接断开，正在重连") + (reason.isEmpty() ? QString() : QStringLiteral(": ") + reason));
    emit connectedChanged(false);
    scheduleReconnect();
}

void MqttLink::checkOutboundAcks()
{
    if (!m_up || m_outbound.isEmpty()) {
        m_ackTimer.stop();
        return;
    }
    const qint64 now = m_clock.elapsed();
    for (auto it = m_outbound.begin(); it != m_outbound.end(); ++it) {
        if (now - it->sentAtMs < kAckTimeoutMs) {
            continue;
        }
        if (it->retries >= kMaxAckRetries) {
            connectionLost(tr("MQTT 发布确认超时"));
            return;
        }
        ++it->retries;
        it->sentAtMs = now;
        // Set the MQTT DUP flag when resending a PUBLISH packet (not PUBREL).
        if (it->awaiting != Mqtt::PubComp && !it->wire.isEmpty()) {
            it->wire[0] = char(quint8(it->wire.at(0)) | 0x08);
        }
        if (!sendPacket(it->wire)) {
            connectionLost(tr("MQTT 发布确认失败"));
            return;
        }
    }
}

void MqttLink::giveUp(const QString &message)
{
    emit errorOccurred(message);
    close();
}

void MqttLink::scheduleReconnect()
{
    if (!m_wanted || m_reconnectTimer.isActive()) {
        return;
    }
    m_reconnectTimer.start(m_reconnectDelayMs);
    m_reconnectDelayMs = std::min(m_reconnectDelayMs * 2, kMaxReconnectDelayMs);
}

} // namespace fhmi
