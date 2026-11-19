#pragma once

#include "comm/link.h"

#include <QtCore/QHash>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <memory>

namespace fhmi {

// Connects out to a device.
class TcpClientLink : public Link
{
    Q_OBJECT

  public:
    explicit TcpClientLink(QObject *parent = nullptr);
    ~TcpClientLink() override;

    void connectTo(const QString &host, quint16 port);
    void close() override;

    QString description() const override;
    bool isConnected() const override;
    bool isConnecting() const;
    bool hasPendingWrite(const QString &) const override { return m_socket.bytesToWrite() > 0; }

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;

  private:
    QTcpSocket m_socket;
    QString m_host;
    quint16 m_port = 0;
};

// Listens for devices. Every connected device gets its own parser and idle timer, so frames
// from different devices never interleave; commands go to every connected device, a slave's reply to the
// device that asked.
class TcpServerLink : public Link
{
    Q_OBJECT

  public:
    explicit TcpServerLink(QObject *parent = nullptr);
    ~TcpServerLink() override;

    bool listen(const QString &address, quint16 port, QString *error = nullptr);
    void close() override;

    QString description() const override;
    bool isConnected() const override; // listening
    bool canSend() const override { return !m_clients.isEmpty(); }
    QStringList sendEndpoints() const override;
    quint64 sendSession(const QString &endpoint) const override;
    bool hasPendingWrite(const QString &endpoint) const override;
    int clientCount() const { return int(m_clients.size()); }

  signals:
    void clientCountChanged(int count);

  protected:
    qint64 writeBytes(const QByteArray &bytes) override;
    qint64 writeReply(Parser *parser, const QByteArray &bytes) override;
    qint64 writeTo(const QByteArray &bytes, const QString &endpoint) override;

  private:
    void onNewConnection();
    void dropClient(QTcpSocket *socket);

    QTcpServer m_server;
    QHash<QTcpSocket *, std::shared_ptr<Parser>> m_clients;
};

} // namespace fhmi
