#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace fhmi {

// MQTT 3.1.1 packets, as far as a client that connects, subscribes and publishes needs them.
namespace Mqtt {

enum PacketType : quint8
{
    Connect = 1,
    ConnAck = 2,
    Publish = 3,
    PubAck = 4,
    PubRec = 5,
    PubRel = 6,
    PubComp = 7,
    Subscribe = 8,
    SubAck = 9,
    Unsubscribe = 10,
    UnsubAck = 11,
    PingReq = 12,
    PingResp = 13,
    Disconnect = 14,
};

struct Packet
{
    quint8 type = 0;
    quint8 flags = 0; // low nibble of the fixed header
    QByteArray body;  // variable header and payload
};

struct ConnectOptions
{
    QString clientId;
    QString username;
    QString password;
    quint16 keepAlive = 60;
    bool cleanSession = true;
};

struct PublishMessage
{
    QString topic;
    QByteArray payload;
    int qos = 0;
    quint16 packetId = 0;
    bool retain = false;
    bool duplicate = false;
};

QByteArray encodeConnect(const ConnectOptions &options);
QByteArray encodePublish(const PublishMessage &message);
QByteArray encodeSubscribe(quint16 packetId, const QStringList &topics, int qos);
// PUBACK, PUBREC, PUBREL and PUBCOMP: a packet identifier and nothing else.
QByteArray encodeAck(PacketType type, quint16 packetId);
QByteArray encodePingReq();
QByteArray encodeDisconnect();

std::optional<PublishMessage> decodePublish(const Packet &packet);
// The 16-bit identifier at the start of an acknowledgement's body.
std::optional<quint16> packetId(const Packet &packet);

// Splits a byte stream into whole packets, keeping a partial one for the next feed.
class StreamDecoder
{
  public:
    QList<Packet> feed(const QByteArray &data);
    void reset() { m_buf.clear(); m_error = false; }
    // A malformed length makes the rest of the stream unreadable.
    bool hasError() const { return m_error; }

  private:
    QByteArray m_buf;
    bool m_error = false;
};

// Why a broker refused CONNECT (return code 1-5), in the user's language.
QString connAckText(int returnCode);

// Topic filters from a list, or text separated by newlines, commas or semicolons; no duplicates.
QStringList topics(const QString &text);

} // namespace Mqtt

} // namespace fhmi
