#pragma once

#include "comm/link.h"
#include "comm/modbus_master.h"
#include "core/command_encoder.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>

namespace fhmi {

class ModbusSlave;

// How sendModbus() queues a request on the links that parse Modbus.
struct ModbusSend
{
    int timeoutMs = 1000;
    ModbusMaster::Priority priority = ModbusMaster::Priority::Write;
    ModbusMaster::Done done; // once per link that queued the request
    const void *owner = nullptr; // for ModbusMaster::cancelQueued()
    bool modbusLinksOnly = false; // leave out the links that do not parse Modbus
    QList<QPointer<ModbusMaster>> *queuedOn = nullptr; // collects the masters that queued it
};

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
    // A link that parses Modbus queues the request in its master (a write by default, with a 1000 ms
    // timeout), which frames it as MBAP, ASCII or RTU; a link that answers as a Modbus slave is left out; any
    // other link is sent the RTU frame right away. A queued request counts as accepted.
    int sendModbus(const ModbusRequest &request, QList<QByteArray> *sent = nullptr, const ModbusSend &how = {});
    int send(const EncodedCommand &command, QList<QByteArray> *sent = nullptr);

    void disconnectAll();

    // The slaves of the link cards whose role is 从站, in the order of the document; the hub does not own them.
    // A slave's change is emitted as messageReceived(), as if a link had received it, with isSlaveData() true
    // meanwhile so the log leaves it out; its log lines come as slaveLog().
    void addModbusSlave(ModbusSlave *slave);
    QList<ModbusSlave *> modbusSlaves() const;
    bool isSlaveData() const { return m_slaveData; }
    // Emits each slave's start messages: every address of every area it has.
    void deliverSlaveStart();

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
    void slaveLog(const QString &text, const QString &level);
    // A slave was added or removed.
    void slavesChanged();
    void connectionStateChanged(bool anyConnected);
    // Which links can send may have changed: one was added or removed, connected or disconnected, or a
    // listening server got its first device or lost its last.
    void linksChanged();

  private:
    void deliverSlaveData(const Message &message);

    QList<QPointer<Link>> m_links;
    QList<QPointer<ModbusSlave>> m_slaves;
    bool m_slaveData = false;
    QJsonObject m_projectSerial;
    QString m_parserType = QStringLiteral("line");
    QJsonObject m_parserConfig;
    QString m_rawEncoding = QStringLiteral("utf-8");
};

} // namespace fhmi
