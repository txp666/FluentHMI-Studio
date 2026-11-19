#pragma once

#include "comm/link.h"
#include "comm/modbus_master.h"
#include "core/command_encoder.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>

namespace fhmi {

class CanLink;
class ModbusSlave;
class VirtualSerialPair;
struct CanFrame;

struct SendTarget
{
    QPointer<Link> link;
    QString endpoint;
    quint64 session = 0;

    bool operator==(const SendTarget &other) const
    {
        return link == other.link && endpoint == other.endpoint && session == other.session;
    }
    QString label() const;
    bool isCurrent() const { return link && session != 0 && link->sendSession(endpoint) == session; }
};

// Every link of a running surface reports here. Bindings, charts, tables and logs
// listen to the hub, and commands go out over the selected connections.
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
    VirtualSerialPair *virtualSerialPair() const { return m_virtualSerialPair; }
    QString mqttTopicSession();

    // The hub does not own links; their cards do.
    void addLink(Link *link, const QString &connectionId = {});
    void removeLink(Link *link);
    QList<Link *> connectedLinks(const DataRoute &route = {}) const;
    bool isConnected() const;
    // The connected links of each kind: CAN links take CAN commands and frames, the others bytes and Modbus.
    QList<CanLink *> connectedCanLinks(const DataRoute &route = {}) const;
    QList<SendTarget> sendTargets(const EncodedCommand &command) const;

    // Returns how many destinations accepted the bytes; `sent` collects the frames they accepted.
    int send(const QByteArray &bytes, QList<QByteArray> *sent = nullptr, const DataRoute &route = {});
    // A link that parses Modbus queues the request in its master (a write by default, with a 1000 ms
    // timeout), which frames it as MBAP, ASCII or RTU; a link that answers as a Modbus slave is left out; any
    // other link is sent the RTU frame right away. A queued request counts as accepted. CAN links are left out.
    int sendModbus(const ModbusRequest &request, QList<QByteArray> *sent = nullptr, const DataRoute &route = {});
    // A CAN command goes to every connected CAN link, each resolving it with its own signals; when one cannot
    // (an unknown signal, a value out of range), *error says why and no link sends anything.
    int sendCan(const CanCommand &command, QList<QByteArray> *sent = nullptr, QString *error = nullptr,
                const DataRoute &route = {});
    // A frame typed in a DataSendCard, to every connected CAN link.
    int sendCanFrame(const CanFrame &frame, QString *error = nullptr, const DataRoute &route = {});
    int send(const EncodedCommand &command, const QList<SendTarget> &targets, QList<QByteArray> *sent = nullptr,
             QString *error = nullptr);

    void disconnectAll();

    // The slaves of the link cards whose role is 从站, in the order of the document; the hub does not own them.
    // A slave's change is emitted as messageReceived(), with source.slaveData true
    // meanwhile so the log leaves it out; its log lines come as slaveLog().
    void addModbusSlave(ModbusSlave *slave, const QString &connectionId = {});
    QList<ModbusSlave *> modbusSlaves(const DataRoute &route = {}) const;
    DataSource modbusSlaveSource(ModbusSlave *slave) const { return m_slaveSources.value(slave); }
    // Emits each slave's start messages: every address of every area it has.
    void deliverSlaveStart();

    QString decodeRaw(const QByteArray &bytes, const DataSource &source) const;
    Link *sourceLink(const DataSource &source) const;

  signals:
    void messageReceived(const fhmi::Message &message, const fhmi::DataSource &source = {});
    void rawReceived(const QByteArray &bytes, const fhmi::DataSource &source = {});
    void unitReceived(const QByteArray &bytes, const fhmi::Message &message, const fhmi::DataSource &source = {});
    void wireReceived(const QByteArray &bytes, const fhmi::DataSource &source);
    void frameRejected(const fhmi::FrameNotice &notice, const fhmi::DataSource &source);
    void bytesSent(const QByteArray &bytes, const fhmi::DataSource &source = {});
    void streamClosed(const fhmi::DataSource &source);
    void linkConnected(const QString &description, const fhmi::DataSource &source = {});
    void linkDisconnected(const QString &description, const fhmi::DataSource &source = {});
    void linkError(const QString &message, const fhmi::DataSource &source = {});
    void linkWarning(const QString &message, const fhmi::DataSource &source = {});
    void linkInfo(const QString &message, const fhmi::DataSource &source = {});
    // A CAN link's frames, received or sent, in batches: the monitor's feed.
    void canFrames(fhmi::CanLink *link, const QList<fhmi::CanFrame> &frames, bool sent, const fhmi::DataSource &source = {});
    void canFrameDecoded(const fhmi::CanFrame &frame, const fhmi::Message &message, const fhmi::DataSource &source);
    // A CAN link connected; the monitor starts afresh when it is the only one connected. Raised here so that a
    // monitor attached before or after the card that creates the link hears it alike.
    void canLinkConnected(fhmi::CanLink *link);
    void canPeriodicResult(quint64 task, fhmi::SendStatus status, const QString &error);
    void canLegacyCyclesChanged();
    void slaveLog(const QString &text, const QString &level, const fhmi::DataSource &source = {});
    // A slave was added or removed.
    void slavesChanged();
    void connectionStateChanged(bool anyConnected);
    // Which links can send may have changed: one was added or removed, connected or disconnected, or a
    // listening server got its first device or lost its last.
    void linksChanged();

  private:
    void deliverSlaveData(const Message &message, ModbusSlave *slave);
    QByteArray sendModbusTo(const ModbusRequest &request, const SendTarget &target);
    int sendCanTo(const EncodedCommand &command, const QList<SendTarget> &targets, QList<QByteArray> *sent,
                  QString *error);

    QList<QPointer<Link>> m_links;
    QList<QPointer<ModbusSlave>> m_slaves;
    QHash<ModbusSlave *, DataSource> m_slaveSources;
    QJsonObject m_projectSerial;
    QString m_parserType = QStringLiteral("line");
    QJsonObject m_parserConfig;
    VirtualSerialPair *m_virtualSerialPair;
    QString m_mqttTopicSession;
};

} // namespace fhmi
