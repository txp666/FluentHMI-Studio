#pragma once

#include "comm/link.h"

#include <QtCore/QHash>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>

#include <memory>

namespace fhmi {

// UDP as a link: bound to a local port, sending to a remote address, or, with none set, to whoever sent last.
// Every datagram is one complete read, as an MQTT message is: a line-mode datagram without its record terminator
// is still parsed, and a parser that frames on silence is flushed after each datagram. Each sender (address and
// port) is a stream with its own parser, as each device of a TCP server is, and a slave's reply goes back to the
// sender that asked. Socket errors after opening (an ICMP "port unreachable") are reported and the link stays open.
class UdpLink : public Link
{
    Q_OBJECT

  public:
    explicit UdpLink(QObject *parent = nullptr);
    ~UdpLink() override;

    // Checks the settings, resolves the remote host and binds the local port (0: the system picks one);
    // connectedChanged(true) follows at once. An empty host replies to the last sender. Returns false, with
    // *error set, when the settings are unusable or the port cannot be bound.
    bool open(quint16 localPort, const QString &host, int port, QString *error = nullptr);
    void close() override;

    QString description() const override;
    bool isConnected() const override { return m_open; }
    // A remote is set, or a datagram has arrived to reply to.
    bool canSend() const override { return m_open && (!m_remote.isNull() || m_hasLastSender); }
    quint16 localPort() const { return m_socket.localPort(); }

    static constexpr qsizetype MaxDatagram = 65507;

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;
    qint64 writeReply(Parser *parser, const QByteArray &bytes) override;

  private:
    struct Stream
    {
        QHostAddress address;
        quint16 port = 0;
        std::shared_ptr<Parser> parser;
    };

    void onReadyRead();
    void receive(const QHostAddress &sender, quint16 senderPort, const QByteArray &data);
    void dropStreams();
    qint64 writeTo(const QByteArray &bytes, const QHostAddress &address, quint16 port);

    QUdpSocket m_socket;
    bool m_open = false;
    bool m_writing = false; // a failed write is reported by writeBytes(), not as a socket error
    QString m_host;         // as entered, for the description
    QHostAddress m_remote;  // null: reply to the last sender
    quint16 m_remotePort = 0;
    bool m_hasLastSender = false;
    QHostAddress m_lastAddress;
    quint16 m_lastPort = 0;
    QHash<QString, Stream> m_streams; // by "address|port"
};

} // namespace fhmi
