#pragma once

#include "core/protocol_parser.h"

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

    // Writes to the device; false when nothing could be written.
    bool send(const QByteArray &bytes);

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
    ModbusMaster *modbusMaster() const { return m_modbusMaster; }

  signals:
    void connectedChanged(bool connected);
    // canSend() changed while the link stayed connected: a listening server got its first device or lost its last.
    void canSendChanged();
    void messageReceived(const fhmi::Message &message);
    void rawReceived(const QByteArray &bytes);
    void bytesSent(const QByteArray &bytes);
    void errorOccurred(const QString &message);
    void infoMessage(const QString &message);

  protected:
    // writeBytes() result for a failure the link already explained through errorOccurred().
    static constexpr qint64 WriteFailureReported = -2;
    virtual qint64 writeBytes(const QByteArray &bytes) = 0;
    // A fresh parser for one incoming stream (a TCP server keeps one per client).
    std::unique_ptr<Parser> createStreamParser() const;
    // Frames bytes read from one stream and reports each complete unit: rawReceived() with
    // the unit, then messageReceived() for each of its messages. A parser that frames on
    // silence gets an idle timer, restarted by every read, that reports what flush() returns.
    void deliver(Parser *parser, const QByteArray &bytes);
    void deliverUnits(Parser *parser, const QList<QByteArray> &units);
    // Stops and forgets the idle timer of a parser about to be destroyed, and the slave replies still due to it.
    void dropStream(Parser *parser);
    // Writes a slave's reply to the stream `parser` frames (a server's device); by default to the link.
    virtual qint64 writeReply(Parser *parser, const QByteArray &bytes);
    Parser *defaultParser() const { return m_parser.get(); }
    void resetDefaultParser();

  private:
    struct PendingReply
    {
        Parser *stream = nullptr;
        QByteArray bytes;
        std::shared_ptr<ModbusSlaveAnswer> answer;
        qint64 due = 0; // ms since the epoch: the request's arrival plus modbusDelay
    };

    std::unique_ptr<Parser> makeParser(const QString &type, const QJsonObject &config, QString *error) const;
    void answer(Parser *parser, const QByteArray &frame);
    void sendNextReply();
    void dropReplies(Parser *parser);

    QString m_parserType = QStringLiteral("line");
    QJsonObject m_parserConfig{{QStringLiteral("separator"), QStringLiteral(",")}};
    std::shared_ptr<ModbusRequests> m_modbusRequests = std::make_shared<ModbusRequests>();
    std::unique_ptr<Parser> m_parser;
    QHash<Parser *, QTimer *> m_idleTimers;
    ModbusMaster *m_modbusMaster = nullptr;
    QPointer<ModbusSlave> m_slave;
    QList<PendingReply> m_replies; // oldest first; the first goes out when m_replyTimer fires
    QTimer *m_replyTimer = nullptr;
};

} // namespace fhmi
