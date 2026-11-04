#include "comm/link.h"

#include "comm/modbus_master.h"
#include "comm/modbus_slave.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>

namespace fhmi {

Link::Link(QObject *parent)
    : QObject(parent), m_parser(createParser(m_parserType, m_parserConfig, nullptr, m_modbusRequests))
{
    m_modbusMaster = new ModbusMaster(this); // first to see each message, so a reply sends the next request
}

Link::~Link() = default;

bool Link::send(const QByteArray &bytes)
{
    if (!canSend() || bytes.isEmpty()) {
        return false;
    }
    m_modbusRequests->noteFrame(bytes, m_parserType); // before a fast reply can arrive
    const qint64 written = writeBytes(bytes);
    if (written == WriteFailureReported) {
        return false;
    }
    if (written != bytes.size()) {
        emit errorOccurred(written < 0 ? tr("写入失败") : tr("只写入了 %1/%2 字节").arg(written).arg(bytes.size()));
        return false;
    }
    emit bytesSent(bytes);
    return true;
}

std::unique_ptr<Parser> Link::makeParser(const QString &type, const QJsonObject &config, QString *error) const
{
    if (m_slave && type == QLatin1String("modbus")) {
        return std::make_unique<ModbusRtuRequestFramer>();
    }
    if (m_slave && type == QLatin1String("modbus_tcp")) {
        return std::make_unique<ModbusTcpRequestFramer>();
    }
    return createParser(type, config, error, m_modbusRequests);
}

bool Link::setParser(const QString &type, const QJsonObject &config, QString *error)
{
    auto parser = makeParser(type, config, error);
    if (!parser) {
        return false;
    }
    m_parserType = type;
    m_parserConfig = config;
    dropStream(m_parser.get());
    m_parser = std::move(parser);
    return true;
}

QString Link::textEncoding() const
{
    if (m_parserType != QLatin1String("line")) {
        return QStringLiteral("utf-8");
    }
    return m_parserConfig.value(QStringLiteral("encoding")).toString(QStringLiteral("utf-8")).toLower();
}

bool Link::parsesModbus() const
{
    return isModbusParser(m_parserType) && !answersModbus();
}

void Link::setModbusSlave(ModbusSlave *slave)
{
    m_slave = slave;
}

ModbusSlave *Link::modbusSlave() const
{
    return m_slave;
}

bool Link::answersModbus() const
{
    return m_slave && (m_parserType == QLatin1String("modbus") || m_parserType == QLatin1String("modbus_tcp"));
}

std::unique_ptr<Parser> Link::createStreamParser() const
{
    return makeParser(m_parserType, m_parserConfig, nullptr);
}

void Link::deliver(Parser *parser, const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    if (!parser) {
        emit rawReceived(bytes);
        return;
    }
    if (const int idle = parser->idleTimeout(); idle > 0) {
        QTimer *&timer = m_idleTimers[parser];
        if (!timer) {
            timer = new QTimer(this);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, this, [this, parser]() { deliverUnits(parser, parser->flush()); });
        }
        timer->start(idle);
    }
    deliverUnits(parser, parser->frames(bytes));
}

void Link::deliverUnits(Parser *parser, const QList<QByteArray> &units)
{
    if (answersModbus()) {
        // Every request is answered, and its reply queued, before anything is emitted: a slot may drop the
        // stream, and its replies with it. A reply goes out after its request was reported.
        for (const QByteArray &unit : units) {
            answer(parser, unit);
        }
        for (const QByteArray &unit : units) {
            emit rawReceived(unit);
        }
        sendNextReply();
        return;
    }
    // Everything is parsed before anything is emitted: a slot may replace or drop the parser.
    QList<QList<Message>> messages;
    for (const QByteArray &unit : units) {
        messages.append(parser->parse(unit));
    }
    for (int i = 0; i < units.size(); ++i) {
        emit rawReceived(units.at(i));
        for (const Message &message : messages.at(i)) {
            emit messageReceived(message);
        }
    }
}

void Link::dropStream(Parser *parser)
{
    if (QTimer *timer = m_idleTimers.take(parser)) {
        timer->stop();
        timer->deleteLater(); // this may run inside the timer's own timeout
    }
    dropReplies(parser);
}

qint64 Link::writeReply(Parser *, const QByteArray &bytes)
{
    return writeBytes(bytes);
}

void Link::answer(Parser *parser, const QByteArray &frame)
{
    const ModbusFraming framing = m_parserType == QLatin1String("modbus_tcp") ? ModbusFraming::Tcp : ModbusFraming::Rtu;
    auto result = std::make_shared<ModbusSlaveAnswer>(m_slave->request(framing, frame));
    if (!result->reply.isEmpty()) {
        const qint64 delay = m_slave->settings().delayMs;
        m_replies.append({parser, result->reply, result, QDateTime::currentMSecsSinceEpoch() + delay});
    }
}

void Link::sendNextReply()
{
    // Each reply is due modbusDelay after its request came in; they go out one at a time, in the order the
    // requests came in.
    if (m_replies.isEmpty() || (m_replyTimer && m_replyTimer->isActive())) {
        return;
    }
    if (!m_replyTimer) {
        m_replyTimer = new QTimer(this);
        m_replyTimer->setSingleShot(true);
        connect(m_replyTimer, &QTimer::timeout, this, [this]() {
            if (m_replies.isEmpty()) {
                return;
            }
            const PendingReply reply = m_replies.takeFirst();
            if (writeReply(reply.stream, reply.bytes) == reply.bytes.size()) {
                emit bytesSent(reply.bytes);
                if (m_slave) {
                    m_slave->replied(*reply.answer);
                }
            }
            sendNextReply();
        });
    }
    m_replyTimer->start(int(qMax<qint64>(0, m_replies.first().due - QDateTime::currentMSecsSinceEpoch())));
}

void Link::dropReplies(Parser *parser)
{
    m_replies.removeIf([parser](const PendingReply &reply) { return reply.stream == parser; });
    if (m_replies.isEmpty() && m_replyTimer) {
        m_replyTimer->stop();
    }
}

void Link::resetDefaultParser()
{
    dropReplies(m_parser.get());
    if (m_parser) {
        m_parser->reset();
        if (QTimer *timer = m_idleTimers.value(m_parser.get())) {
            timer->stop(); // nothing left to flush
        }
    }
}

} // namespace fhmi
