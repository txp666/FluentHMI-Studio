#include "comm/tcp_link.h"

#include <QtNetwork/QHostAddress>

namespace fhmi {

// ---------------------------------------------------------------------------
// TcpClientLink
// ---------------------------------------------------------------------------

TcpClientLink::TcpClientLink(QObject *parent) : Link(parent)
{
    connect(&m_socket, &QTcpSocket::connected, this, [this]() {
        resetDefaultParser();
        emit connectedChanged(true);
    });
    connect(&m_socket, &QTcpSocket::disconnected, this, [this]() {
        resetDefaultParser();
        emit connectedChanged(false);
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, [this]() { deliver(defaultParser(), m_socket.readAll()); });
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (error != QAbstractSocket::RemoteHostClosedError) {
            emit errorOccurred(m_socket.errorString());
        }
    });
}

TcpClientLink::~TcpClientLink()
{
    m_socket.blockSignals(true);
    m_socket.abort();
}

void TcpClientLink::connectTo(const QString &host, quint16 port)
{
    m_socket.abort();
    m_host = host;
    m_port = port;
    m_socket.connectToHost(host, port);
}

void TcpClientLink::close()
{
    if (m_socket.state() == QAbstractSocket::UnconnectedState) {
        return;
    }
    const bool wasConnected = isConnected();
    // abort() reports disconnected() itself only once connected, so a pending
    // connection attempt is cancelled quietly.
    m_socket.abort();
    if (!wasConnected) {
        emit connectedChanged(false);
    }
}

QString TcpClientLink::description() const
{
    return QStringLiteral("TCP %1:%2").arg(m_host).arg(m_port);
}

bool TcpClientLink::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

bool TcpClientLink::isConnecting() const
{
    return m_socket.state() == QAbstractSocket::HostLookupState ||
           m_socket.state() == QAbstractSocket::ConnectingState;
}

qint64 TcpClientLink::writeBytes(const QByteArray &bytes)
{
    return m_socket.write(bytes);
}

// ---------------------------------------------------------------------------
// TcpServerLink
// ---------------------------------------------------------------------------

TcpServerLink::TcpServerLink(QObject *parent) : Link(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &TcpServerLink::onNewConnection);
}

TcpServerLink::~TcpServerLink()
{
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        it.key()->blockSignals(true);
        it.key()->abort();
        delete it.key();
    }
    m_clients.clear();
}

bool TcpServerLink::listen(const QString &address, quint16 port, QString *error)
{
    close();
    const QHostAddress host = address.trimmed().isEmpty() || address == QLatin1String("0.0.0.0")
                                  ? QHostAddress(QHostAddress::AnyIPv4)
                                  : QHostAddress(address.trimmed());
    if (!m_server.listen(host, port)) {
        if (error) {
            *error = m_server.errorString();
        }
        return false;
    }
    emit connectedChanged(true);
    emit infoMessage(tr("正在监听 %1:%2").arg(host.toString()).arg(m_server.serverPort()));
    return true;
}

void TcpServerLink::close()
{
    const bool wasListening = m_server.isListening();
    m_server.close();
    const QList<QTcpSocket *> sockets = m_clients.keys();
    for (QTcpSocket *socket : sockets) {
        dropClient(socket);
    }
    if (wasListening) {
        emit connectedChanged(false);
    }
}

QString TcpServerLink::description() const
{
    return QStringLiteral("TCP 服务端 :%1").arg(m_server.serverPort());
}

bool TcpServerLink::isConnected() const
{
    return m_server.isListening();
}

qint64 TcpServerLink::writeBytes(const QByteArray &bytes)
{
    if (m_clients.isEmpty()) {
        return -1;
    }
    qint64 accepted = -1;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        if (it.key()->write(bytes) == bytes.size()) {
            accepted = bytes.size();
        }
    }
    return accepted;
}

void TcpServerLink::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        m_clients.insert(socket, std::shared_ptr<Parser>(createStreamParser()));
        const QString peer = QStringLiteral("%1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort());
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            const auto it = m_clients.constFind(socket);
            if (it != m_clients.constEnd()) {
                deliver(it.value().get(), socket->readAll());
            }
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket, peer]() {
            emit infoMessage(tr("设备已断开: %1").arg(peer));
            dropClient(socket);
        });
        emit infoMessage(tr("设备已连入: %1").arg(peer));
        emit clientCountChanged(clientCount());
    }
}

void TcpServerLink::dropClient(QTcpSocket *socket)
{
    if (!m_clients.remove(socket)) {
        return;
    }
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
    emit clientCountChanged(clientCount());
}

} // namespace fhmi
