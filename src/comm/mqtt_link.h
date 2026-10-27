#pragma once

#include "comm/link.h"
#include "core/mqtt_codec.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtNetwork/QSslSocket>

#include <memory>

namespace fhmi {

struct MqttSettings
{
    QString host = QStringLiteral("127.0.0.1");
    int port = 1883;
    QString clientId; // empty: generated
    QString username;
    QString password;
    QStringList topics;   // subscribed with `qos`; + and # wildcards are the broker's business
    QString publishTopic; // where commands go
    int qos = 0;
    bool tls = false;
    quint16 keepAlive = 60;
};

// An MQTT 3.1.1 client as a link. Every message is one complete frame: a line-mode
// payload without a trailing newline is still parsed, a bare value is also reported under
// its topic name (so it can be bound by topic), and HEX frames buffer per topic. Commands
// are published to the publish topic. After a connection that worked drops, the link keeps
// reconnecting until it is closed; a first attempt that fails is reported and given up.
class MqttLink : public Link
{
    Q_OBJECT

  public:
    explicit MqttLink(QObject *parent = nullptr);
    ~MqttLink() override;

    // Checks the settings and starts connecting; connectedChanged(true) follows once the
    // broker accepts. Returns false, with *error set, for unusable settings.
    bool connectTo(const MqttSettings &settings, QString *error = nullptr);
    void close() override;

    QString description() const override;
    bool isConnected() const override { return m_up; }
    // Connecting, connected or waiting to reconnect.
    bool isActive() const { return m_wanted; }
    // The connection worked, dropped, and is being re-established.
    bool isReconnecting() const { return m_wanted && !m_up && m_everUp; }
    const MqttSettings &settings() const { return m_settings; }

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;

  private:
    void openSocket();
    void sendConnect();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);
    void onSocketDisconnected();
    void handlePacket(const Mqtt::Packet &packet);
    void handleMessage(const Mqtt::PublishMessage &message);
    void connectionLost(const QString &reason);
    void giveUp(const QString &message);
    void scheduleReconnect();
    bool sendPacket(const QByteArray &packet);
    quint16 nextPacketId();

    QSslSocket m_socket;
    Mqtt::StreamDecoder m_decoder;
    MqttSettings m_settings;
    bool m_wanted = false; // the user asked for a connection
    bool m_up = false;     // the broker accepted CONNECT
    bool m_everUp = false; // reached the broker since connectTo()
    quint16 m_lastPacketId = 0;
    QHash<quint16, QStringList> m_pendingSubscriptions;
    QSet<quint16> m_inboundQos2; // delivered, waiting for PUBREL
    QSet<quint16> m_outbound;    // published with QoS 1/2, not yet acknowledged
    QHash<QString, std::shared_ptr<Parser>> m_parsers;
    QTimer m_pingTimer;
    QTimer m_reconnectTimer;
    QElapsedTimer m_sinceReceived;
    int m_reconnectDelayMs = 1000;
};

} // namespace fhmi
