#include "comm/udp_link.h"

#include <QtNetwork/QHostInfo>
#include <QtNetwork/QNetworkDatagram>

namespace fhmi {

namespace {

// Senders beyond this are dropped and their streams rebuilt, as MQTT topics are.
constexpr int kMaxStreams = 256;
// Room for a datagram of MaxDatagram bytes and its headers.
constexpr int kSendBuffer = 65536 + 1024;

QString streamKey(const QHostAddress &address, quint16 port)
{
    return address.toString() + QLatin1Char('|') + QString::number(port);
}

} // namespace

UdpLink::UdpLink(QObject *parent) : Link(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &UdpLink::onReadyRead);
    connect(&m_socket, &QUdpSocket::errorOccurred, this, [this]() {
        if (m_open && !m_writing) {
            emit errorOccurred(tr("UDP 错误: %1").arg(m_socket.errorString()));
        }
    });
}

UdpLink::~UdpLink()
{
    m_socket.blockSignals(true);
    m_socket.abort();
}

bool UdpLink::open(quint16 localPort, const QString &host, int port, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    const auto invalid = [&fail](const QString &message) { return fail(tr("UDP 参数无效: %1").arg(message)); };
    const QString name = host.trimmed();
    if (name.isEmpty() && localPort == 0) {
        return invalid(tr("请填写远程地址或本地端口"));
    }
    QHostAddress remote;
    if (!name.isEmpty()) {
        if (port < 1 || port > 65535) {
            return invalid(tr("端口必须在 1–65535 之间"));
        }
        if (!remote.setAddress(name)) {
            // A name is looked up once, here; the first IPv4 address, else the first address.
            const QList<QHostAddress> found = QHostInfo::fromName(name).addresses();
            for (const QHostAddress &address : found) {
                if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                    remote = address;
                    break;
                }
            }
            if (remote.isNull() && !found.isEmpty()) {
                remote = found.first();
            }
            if (remote.isNull()) {
                return invalid(tr("无法解析地址 %1").arg(name));
            }
        }
    }

    close();
    const bool ipv6 = remote.protocol() == QAbstractSocket::IPv6Protocol;
    if (!m_socket.bind(ipv6 ? QHostAddress(QHostAddress::Any) : QHostAddress(QHostAddress::AnyIPv4), localPort)) {
        const QString reason = m_socket.errorString();
        m_socket.abort();
        return fail(tr("UDP 绑定失败: %1").arg(reason));
    }
    // macOS sends no datagram over 9216 bytes with its default send buffer; make room for the largest one.
    if (m_socket.socketOption(QAbstractSocket::SendBufferSizeSocketOption).toInt() < kSendBuffer) {
        m_socket.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, kSendBuffer);
    }
    m_host = name;
    m_remote = remote;
    m_remotePort = name.isEmpty() ? 0 : quint16(port);
    m_open = true;
    emit connectedChanged(true);
    return true;
}

void UdpLink::close()
{
    const bool wasOpen = m_open;
    m_open = false;
    m_socket.blockSignals(true);
    m_socket.abort();
    m_socket.blockSignals(false);
    dropStreams();
    m_hasLastSender = false;
    m_lastAddress.clear();
    m_lastPort = 0;
    if (wasOpen) {
        emit connectedChanged(false);
    }
}

QString UdpLink::description() const
{
    const QString remote = m_remote.isNull() ? tr("最后发送方") : QStringLiteral("%1:%2").arg(m_host).arg(m_remotePort);
    return QStringLiteral("UDP :%1 → %2").arg(m_socket.localPort()).arg(remote);
}

qint64 UdpLink::writeBytes(const QByteArray &bytes)
{
    if (bytes.size() > MaxDatagram) {
        emit errorOccurred(tr("UDP 数据报过大: %1 字节（最多 65507）").arg(bytes.size()));
        return WriteFailureReported;
    }
    if (!m_remote.isNull()) {
        return writeTo(bytes, m_remote, m_remotePort);
    }
    if (!m_hasLastSender) {
        return WriteFailureReported; // nobody to reply to yet; canSend() said so
    }
    return writeTo(bytes, m_lastAddress, m_lastPort);
}

qint64 UdpLink::writeReply(Parser *parser, const QByteArray &bytes)
{
    // A slave's reply goes back to the sender that asked, never to the remote.
    for (auto it = m_streams.constBegin(); it != m_streams.constEnd(); ++it) {
        if (it->parser.get() == parser) {
            return writeTo(bytes, it->address, it->port);
        }
    }
    return -1; // the stream is gone
}

qint64 UdpLink::writeTo(const QByteArray &bytes, const QHostAddress &address, quint16 port)
{
    m_writing = true;
    const qint64 written = m_socket.writeDatagram(bytes, address, port);
    m_writing = false;
    if (written != bytes.size()) {
        emit errorOccurred(tr("发送失败: %1").arg(m_socket.errorString()));
        return WriteFailureReported;
    }
    return written;
}

void UdpLink::onReadyRead()
{
    while (m_open && m_socket.hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket.receiveDatagram();
        if (!datagram.isValid()) {
            break; // an error the socket reports itself
        }
        receive(datagram.senderAddress(), quint16(datagram.senderPort()), datagram.data());
    }
}

void UdpLink::receive(const QHostAddress &sender, quint16 senderPort, const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }
    const bool firstSender = !m_hasLastSender;
    m_hasLastSender = true;
    m_lastAddress = sender;
    m_lastPort = senderPort;
    if (firstSender && m_remote.isNull()) {
        emit canSendChanged(); // commands can go to this sender now
    }

    const QString key = streamKey(sender, senderPort);
    auto it = m_streams.find(key);
    if (it == m_streams.end()) {
        if (m_streams.size() >= kMaxStreams) {
            dropStreams();
        }
        it = m_streams.insert(key, {sender, senderPort, std::shared_ptr<Parser>(createStreamParser())});
    }
    const std::shared_ptr<Parser> parser = it->parser; // a slot may close the link and drop the streams
    if (!parser) {
        emit rawReceived(data);
        return;
    }
    // Each datagram is a whole record: a line parser gets its terminator if it is missing.
    const QByteArray terminator = parserType() == QLatin1String("line") ? lineTerminator(parserConfig()) : QByteArray();
    const auto report = [this, &parser, &terminator](const QList<QByteArray> &units) {
        if (terminator.isEmpty()) {
            deliverUnits(parser.get(), units);
            return;
        }
        QList<QList<Message>> messages; // everything parsed before anything is emitted
        for (const QByteArray &unit : units) {
            messages.append(parser->parse(unit.endsWith(terminator) ? unit : unit + terminator));
        }
        for (int i = 0; i < units.size(); ++i) {
            emit rawReceived(units.at(i));
            for (const Message &message : messages.at(i)) {
                emit messageReceived(message);
            }
        }
    };
    report(parser->frames(data));
    // A datagram arrives whole, so a parser that frames on silence has its frame now.
    if (parser->idleTimeout() > 0) {
        report(parser->flush());
    }
}

void UdpLink::dropStreams()
{
    for (const Stream &stream : std::as_const(m_streams)) {
        dropStream(stream.parser.get());
    }
    m_streams.clear();
}

} // namespace fhmi
