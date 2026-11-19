#pragma once

#include "core/protocol_parser.h"
#include "core/data_source.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include <memory>

class QTimer;

namespace fhmi {

class ModbusMaster;
class ModbusSlave;
struct ModbusSlaveAnswer;

enum class SendStatus { Submitted, Busy, Failed };

// One transport to a device (a serial port, a TCP connection, ...). Every link frames
// its incoming bytes with its own parser and reports the same signals, so the hub,
// bindings, charts, tables and logs never care which transport the data came over.
class Link : public QObject
{
    Q_OBJECT

  public:
    explicit Link(QObject *parent = nullptr);
    ~Link() override;

    virtual QString description() const = 0; // "COM3 @ 115200", "TCP 127.0.0.1:8080"
    virtual bool isConnected() const = 0;
    // Whether a write can reach a device right now (a listening server may have none).
    virtual bool canSend() const { return isConnected(); }
    virtual void close() = 0;
    // A CAN link takes CAN commands and frames only: send() does nothing on it, and the hub sends it no bytes.
    virtual bool isCan() const { return false; }

    // Writes to the device; false when nothing could be written.
    bool send(const QByteArray &bytes, const QString &endpoint = {});
    SendStatus sendPeriodic(const QByteArray &bytes, const QString &endpoint);
    void setConnectionId(const QString &id) { m_connectionId = id; }
    QString connectionId() const { return m_connectionId; }
    DataSource dataSource(Parser *parser = nullptr);
    DataSource eventSource(const QString &endpoint = {}) const;
    bool ownsStream(quint64 streamId, const QString &endpoint = {}) const;
    FrameProgress frameProgress(quint64 streamId) const;

    bool setParser(const QString &type, const QJsonObject &config, QString *error = nullptr);
    QString parserType() const { return m_parserType; }
    QJsonObject parserConfig() const { return m_parserConfig; }
    // The encoding of the text this link receives: the line parser's, UTF-8 for every other parser.
    QString textEncoding() const;
    // Whether the link parses Modbus replies (modbus, modbus_tcp or modbus_ascii), so its master can
    // pair each request with its reply. A link that answers as a slave does not.
    bool parsesModbus() const;
    // The slave of a card whose role is 从站; set before setParser(), not owned. While the parser is modbus or
    // modbus_tcp the link answers as that slave instead of parsing replies: it frames requests, and each reply
    // goes back to the stream the request came from, modbusDelay after it, one after another.
    void setModbusSlave(ModbusSlave *slave);
    ModbusSlave *modbusSlave() const;
    bool answersModbus() const;
    // The queue every Modbus request on this link goes through, one on the line at a time.
    ModbusMaster *modbusMaster(const QString &endpoint = {});
    virtual QStringList sendEndpoints() const { return {QString()}; }
    // A named TCP peer must already be connected. UDP and MQTT can address a new destination.
    virtual bool canSendTo(const QString &endpoint) const;
    virtual quint64 sendSession(const QString &endpoint) const;
    virtual bool hasPendingWrite(const QString &) const { return false; }
    // MQTT request/reply topics may differ; their Modbus transactions belong to the connection.
    virtual bool addressedModbusReplies() const { return true; }

  signals:
    void connectedChanged(bool connected);
    // Available destinations changed while the link stayed connected.
    void canSendChanged();
    void messageReceived(const fhmi::Message &message, const fhmi::DataSource &source = {});
    void rawReceived(const QByteArray &bytes, const fhmi::DataSource &source = {});
    void unitReceived(const QByteArray &bytes, const fhmi::Message &message, const fhmi::DataSource &source);
    void wireReceived(const QByteArray &bytes, const fhmi::DataSource &source);
    void frameRejected(const fhmi::FrameNotice &notice, const fhmi::DataSource &source);
    void bytesSent(const QByteArray &bytes, const fhmi::DataSource &source = {});
    void streamClosed(const fhmi::DataSource &source);
    void errorOccurred(const QString &message, const QString &endpoint = {});
    // Something the log shows at warning level (a DBC line skipped, a CAN bus in error passive).
    void warningMessage(const QString &message, const QString &endpoint = {});
    void infoMessage(const QString &message, const QString &endpoint = {});

  protected:
    // writeBytes() result for a failure the link already explained through errorOccurred().
    static constexpr qint64 WriteFailureReported = -2;
    virtual qint64 writeBytes(const QByteArray &bytes) = 0;
    virtual qint64 writeTo(const QByteArray &bytes, const QString &endpoint);
    void registerStream(Parser *parser, const QString &endpoint);
    quint64 streamIdForEndpoint(const QString &endpoint) const;
    // A fresh parser for one incoming stream (a TCP server keeps one per client).
    std::unique_ptr<Parser> createStreamParser(const QString &endpoint = {});
    enum class Boundary { Stream, Message };
    // Binary buffering is per stream. Datagram transports also supply a boundary for line and idle framing.
    void deliver(Parser *parser, const QByteArray &bytes, Boundary boundary = Boundary::Stream);
    virtual void annotateMessage(Message &, const DataSource &) const {}
    // Stops and forgets the idle timer of a parser about to be destroyed, and the slave replies still due to it.
    void dropStream(Parser *parser);
    // Writes a slave's reply to the stream `parser` frames (a server's device); by default to the link.
    virtual qint64 writeReply(Parser *parser, const QByteArray &bytes);
    Parser *defaultParser() const { return m_parser.get(); }
    void resetDefaultParser();

  private:
    friend class ModbusMaster;

    struct PendingReply
    {
        Parser *stream = nullptr;
        QByteArray bytes;
        std::shared_ptr<ModbusSlaveAnswer> answer;
        qint64 due = 0; // ms since the epoch: the request's arrival plus modbusDelay
    };

    std::unique_ptr<Parser> makeParser(const QString &type, const QJsonObject &config, QString *error, const std::shared_ptr<ModbusRequests> &requests) const;
    void deliverUnits(Parser *parser, const QList<QByteArray> &units, Boundary boundary = Boundary::Stream);
    void publishUnit(const QByteArray &bytes, const QList<DecodedRecord> &records, const DataSource &source);
    void reportFramingState(Parser *parser, const DataSource &source);
    void answer(Parser *parser, const QByteArray &frame);
    void sendNextReply();
    void dropReplies(Parser *parser);

    QString m_parserType = QStringLiteral("line");
    QJsonObject m_parserConfig{{QStringLiteral("separator"), QStringLiteral(",")}};
    std::shared_ptr<ModbusRequests> m_modbusRequests = std::make_shared<ModbusRequests>();
    std::unique_ptr<Parser> m_parser;
    QHash<Parser *, QTimer *> m_idleTimers;
    ModbusMaster *m_modbusMaster = nullptr;
    QHash<QString, QPointer<ModbusMaster>> m_endpointMasters;
    QHash<QString, std::shared_ptr<ModbusRequests>> m_endpointRequests;
    std::shared_ptr<ModbusRequests> requestsFor(const QString &endpoint);
    QPointer<ModbusSlave> m_slave;
    QList<PendingReply> m_replies; // oldest first; the first goes out when m_replyTimer fires
    QTimer *m_replyTimer = nullptr;
    struct Stream { quint64 id = nextDataStream(); QString endpoint; };
    QString m_connectionId;
    quint64 m_sendSession = nextDataStream();
    QHash<Parser *, Stream> m_streams;
};

} // namespace fhmi
