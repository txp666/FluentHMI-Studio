#include "core/mqtt_codec.h"

#include <QtCore/QRegularExpression>

namespace fhmi::Mqtt {

namespace {

void appendU16(QByteArray &out, quint16 value)
{
    out.append(char(value >> 8));
    out.append(char(value & 0xFF));
}

void appendString(QByteArray &out, const QByteArray &utf8)
{
    appendU16(out, quint16(utf8.size()));
    out.append(utf8);
}

QByteArray remainingLength(int length)
{
    QByteArray out;
    do {
        quint8 digit = length % 128;
        length /= 128;
        if (length > 0) {
            digit |= 0x80;
        }
        out.append(char(digit));
    } while (length > 0);
    return out;
}

QByteArray packet(quint8 type, quint8 flags, const QByteArray &body)
{
    return QByteArray(1, char((type << 4) | (flags & 0x0F))) + remainingLength(int(body.size())) + body;
}

quint16 readU16(const QByteArray &data, int at)
{
    return quint16((quint8(data.at(at)) << 8) | quint8(data.at(at + 1)));
}

} // namespace

QByteArray encodeConnect(const ConnectOptions &options)
{
    QByteArray body;
    appendString(body, "MQTT");
    body.append(char(4)); // protocol level 3.1.1
    quint8 flags = options.cleanSession ? 0x02 : 0x00;
    if (!options.username.isEmpty()) {
        flags |= 0x80;
        if (!options.password.isEmpty()) {
            flags |= 0x40;
        }
    }
    body.append(char(flags));
    appendU16(body, options.keepAlive);
    appendString(body, options.clientId.toUtf8());
    if (!options.username.isEmpty()) {
        appendString(body, options.username.toUtf8());
        if (!options.password.isEmpty()) {
            appendString(body, options.password.toUtf8());
        }
    }
    return packet(Connect, 0, body);
}

QByteArray encodePublish(const PublishMessage &message)
{
    QByteArray body;
    appendString(body, message.topic.toUtf8());
    if (message.qos > 0) {
        appendU16(body, message.packetId);
    }
    body.append(message.payload);
    const quint8 flags = quint8((message.duplicate ? 0x08 : 0) | ((message.qos & 0x03) << 1) | (message.retain ? 1 : 0));
    return packet(Publish, flags, body);
}

QByteArray encodeSubscribe(quint16 id, const QStringList &topicFilters, int qos)
{
    QByteArray body;
    appendU16(body, id);
    for (const QString &topic : topicFilters) {
        appendString(body, topic.toUtf8());
        body.append(char(qos & 0x03));
    }
    return packet(Subscribe, 0x02, body);
}

QByteArray encodeAck(PacketType type, quint16 id)
{
    QByteArray body;
    appendU16(body, id);
    // PUBREL is the only acknowledgement with a reserved flag set.
    return packet(type, type == PubRel ? 0x02 : 0x00, body);
}

QByteArray encodePingReq()
{
    return packet(PingReq, 0, {});
}

QByteArray encodeDisconnect()
{
    return packet(Disconnect, 0, {});
}

std::optional<PublishMessage> decodePublish(const Packet &p)
{
    if (p.type != Publish || p.body.size() < 2) {
        return std::nullopt;
    }
    PublishMessage message;
    message.qos = (p.flags >> 1) & 0x03;
    message.retain = p.flags & 0x01;
    message.duplicate = p.flags & 0x08;
    if (message.qos > 2) {
        return std::nullopt;
    }
    const int topicLength = readU16(p.body, 0);
    int at = 2 + topicLength;
    if (at > p.body.size()) {
        return std::nullopt;
    }
    message.topic = QString::fromUtf8(p.body.mid(2, topicLength));
    if (message.qos > 0) {
        if (at + 2 > p.body.size()) {
            return std::nullopt;
        }
        message.packetId = readU16(p.body, at);
        at += 2;
    }
    message.payload = p.body.mid(at);
    return message;
}

std::optional<quint16> packetId(const Packet &p)
{
    if (p.body.size() < 2) {
        return std::nullopt;
    }
    return readU16(p.body, 0);
}

QList<Packet> StreamDecoder::feed(const QByteArray &data)
{
    QList<Packet> packets;
    if (m_error) {
        return packets;
    }
    m_buf += data;
    while (m_buf.size() >= 2) {
        int length = 0;
        int multiplier = 1;
        int index = 1;
        bool complete = false;
        while (index < m_buf.size() && index <= 4) {
            const quint8 digit = quint8(m_buf.at(index));
            length += (digit & 0x7F) * multiplier;
            multiplier *= 128;
            ++index;
            if (!(digit & 0x80)) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            if (index > 4) {
                m_error = true; // more than four length bytes
                m_buf.clear();
            }
            break;
        }
        if (m_buf.size() < index + length) {
            break;
        }
        Packet p;
        p.type = quint8(m_buf.at(0)) >> 4;
        p.flags = quint8(m_buf.at(0)) & 0x0F;
        p.body = m_buf.mid(index, length);
        packets.append(p);
        m_buf.remove(0, index + length);
    }
    return packets;
}

QString connAckText(int code)
{
    switch (code) {
    case 1:
        return QStringLiteral("协议版本不支持");
    case 2:
        return QStringLiteral("客户端ID无效");
    case 3:
        return QStringLiteral("服务器不可用");
    case 4:
        return QStringLiteral("用户名或密码错误");
    case 5:
        return QStringLiteral("未授权，请检查用户名和密码");
    default:
        return QStringLiteral("返回码 %1").arg(code);
    }
}

QStringList topics(const QString &text)
{
    static const QRegularExpression separators(QStringLiteral("[\\n,;]"));
    QStringList out;
    for (const QString &part : text.split(separators)) {
        const QString topic = part.trimmed();
        if (!topic.isEmpty() && !out.contains(topic)) {
            out.append(topic);
        }
    }
    return out;
}

} // namespace fhmi::Mqtt
