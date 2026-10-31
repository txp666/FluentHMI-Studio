#pragma once

#include "comm/link.h"
#include "core/command_encoder.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>

namespace fhmi {

// Every link of a running surface reports here. Bindings, charts, tables and logs
// listen to the hub, and commands go out over every connected link.
class CommHub : public QObject
{
    Q_OBJECT

  public:
    explicit CommHub(QObject *parent = nullptr);

    // The project's "serial" section: the default parser every link starts with.
    void configure(const QJsonObject &projectSerial);
    QString defaultParserType() const { return m_parserType; }
    QJsonObject defaultParserConfig() const { return m_parserConfig; }
    QJsonObject projectSerial() const { return m_projectSerial; }

    // The hub does not own links; their cards do.
    void addLink(Link *link);
    void removeLink(Link *link);
    QList<Link *> connectedLinks() const;
    bool isConnected() const;

    // Returns how many links accepted the bytes; `sent` collects the frames they accepted.
    int send(const QByteArray &bytes, QList<QByteArray> *sent = nullptr);
    // Frames the request per link: MBAP where the link parses Modbus TCP, ASCII where it parses
    // Modbus ASCII, RTU elsewhere.
    int sendModbus(const ModbusRequest &request, QList<QByteArray> *sent = nullptr);
    int send(const EncodedCommand &command, QList<QByteArray> *sent = nullptr);

    void disconnectAll();

    // The encoding of the bytes rawReceived() is carrying right now, and those bytes as text:
    // raw bytes carry no link, so the encoding of the link they came from rides alongside.
    QString rawEncoding() const { return m_rawEncoding; }
    QString decodeRaw(const QByteArray &bytes) const;

  signals:
    void messageReceived(const fhmi::Message &message);
    void rawReceived(const QByteArray &bytes);
    void bytesSent(const QByteArray &bytes);
    void linkConnected(const QString &description);
    void linkDisconnected(const QString &description);
    void linkError(const QString &message);
    void linkInfo(const QString &message);
    void connectionStateChanged(bool anyConnected);

  private:
    QList<QPointer<Link>> m_links;
    QJsonObject m_projectSerial;
    QString m_parserType = QStringLiteral("line");
    QJsonObject m_parserConfig;
    QString m_rawEncoding = QStringLiteral("utf-8");
};

} // namespace fhmi
