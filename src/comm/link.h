#pragma once

#include "core/protocol_parser.h"

#include <QtCore/QHash>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

class QTimer;

namespace fhmi {

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

  signals:
    void connectedChanged(bool connected);
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
    // Stops and forgets the idle timer of a parser about to be destroyed.
    void dropStream(Parser *parser);
    Parser *defaultParser() const { return m_parser.get(); }
    void resetDefaultParser();

  private:
    QString m_parserType = QStringLiteral("line");
    QJsonObject m_parserConfig{{QStringLiteral("separator"), QStringLiteral(",")}};
    std::shared_ptr<ModbusRequests> m_modbusRequests = std::make_shared<ModbusRequests>();
    std::unique_ptr<Parser> m_parser;
    QHash<Parser *, QTimer *> m_idleTimers;
};

} // namespace fhmi
