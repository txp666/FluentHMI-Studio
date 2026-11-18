#include "comm/link.h"

#include "comm/modbus_master.h"
#include "comm/modbus_slave.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>

namespace fhmi {

Link::Link(QObject *parent)
    : QObject(parent), m_parser(createParser(m_parserType, m_parserConfig, nullptr, m_modbusRequests))
{
    qRegisterMetaType<DataSource>();
    connect(this, &Link::connectedChanged, this, [this](bool connected) {
        if (!connected) {
            const auto streams = std::exchange(m_streams, {});
            for (const auto &stream : streams) emit streamClosed({m_connectionId, stream.id, stream.endpoint, textEncoding(), isCan(), false});
            m_endpointRequests.clear();
            for (auto master : std::as_const(m_endpointMasters)) if (master) master->deleteLater();
            m_endpointMasters.clear();
            *m_modbusRequests = ModbusRequests{};
            if (m_parser) m_parser->reset();
        }
    });
    m_modbusMaster = new ModbusMaster(this); // first to see each message, so a reply sends the next request
}

Link::~Link() = default;

std::shared_ptr<ModbusRequests> Link::requestsFor(const QString &endpoint)
{
    if (endpoint.isEmpty() || !addressedModbusReplies()) return m_modbusRequests;
    auto &requests = m_endpointRequests[endpoint];
    if (!requests) requests = std::make_shared<ModbusRequests>();
    return requests;
}

ModbusMaster *Link::modbusMaster(const QString &endpoint)
{
    if (endpoint.isEmpty() || !addressedModbusReplies()) return m_modbusMaster;
    auto &master = m_endpointMasters[endpoint];
    if (!master) master = new ModbusMaster(this, endpoint);
    return master;
}

DataSource Link::dataSource(Parser *parser)
{
    const Stream &stream = m_streams[parser ? parser : m_parser.get()];
    return {m_connectionId, stream.id, stream.endpoint, textEncoding(), isCan(), false};
}

bool Link::ownsStream(quint64 streamId, const QString &endpoint) const
{
    for (const auto &stream : m_streams) {
        if (stream.id == streamId && (endpoint.isEmpty() || endpoint == stream.endpoint))
            return true;
    }
    return false;
}

bool Link::canSendTo(const QString &endpoint) const
{
    return isConnected() && canSend() && (endpoint.isEmpty() || sendEndpoints().contains(endpoint));
}

void Link::registerStream(Parser *parser, const QString &endpoint)
{
    dropStream(parser);
    m_streams.insert(parser, Stream{nextDataStream(), endpoint});
}

qint64 Link::writeTo(const QByteArray &bytes, const QString &endpoint)
{
    if (!endpoint.isEmpty()) {
        emit errorOccurred(tr("此连接没有可指定的子通道"));
        return WriteFailureReported;
    }
    return writeBytes(bytes);
}

bool Link::send(const QByteArray &bytes, const QString &endpoint)
{
    if (!isConnected() || bytes.isEmpty()) {
        return false;
    }
    // Each peer owns a request book; unrelated replies must never consume its pending read.
    const auto targets = endpoint.isEmpty() ? sendEndpoints() : QStringList{endpoint};
    if (parsesModbus()) {
        for (const QString &target : targets)
            requestsFor(target)->noteFrame(bytes, m_parserType);
    }
    const qint64 written = writeTo(bytes, endpoint);
    if (written == WriteFailureReported) {
        return false;
    }
    if (written != bytes.size()) {
        emit errorOccurred(written < 0 ? tr("写入失败") : tr("只写入了 %1/%2 字节").arg(written).arg(bytes.size()));
        return false;
    }
    for (const auto &target : targets) {
        auto source = dataSource();
        source.endpoint = target;
        for (auto it = m_streams.cbegin(); it != m_streams.cend(); ++it) {
            if (it->endpoint == target) { source.stream = it->id; break; }
        }
        emit bytesSent(bytes, source);
    }
    return true;
}

std::unique_ptr<Parser> Link::makeParser(const QString &type, const QJsonObject &config, QString *error, const std::shared_ptr<ModbusRequests> &requests) const
{
    if (m_slave && type == QLatin1String("modbus")) {
        return std::make_unique<ModbusRtuRequestFramer>();
    }
    if (m_slave && type == QLatin1String("modbus_tcp")) {
        return std::make_unique<ModbusTcpRequestFramer>();
    }
    return createParser(type, config, error, requests);
}

bool Link::setParser(const QString &type, const QJsonObject &config, QString *error)
{
    auto parser = makeParser(type, config, error, m_modbusRequests);
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

std::unique_ptr<Parser> Link::createStreamParser(const QString &endpoint)
{
    return makeParser(m_parserType, m_parserConfig, nullptr, requestsFor(endpoint));
}

void Link::deliver(Parser *parser, const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    if (!parser) {
        emit rawReceived(bytes, dataSource(parser));
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
            emit rawReceived(unit, dataSource(parser));
        }
        sendNextReply();
        return;
    }
    // Snapshot source as well: a callback can disconnect and replace this stream.
    const DataSource source = dataSource(parser);
    QList<QList<Message>> messages;
    for (const QByteArray &unit : units) {
        messages.append(parser->parse(unit));
    }
    for (int i = 0; i < units.size(); ++i) {
        emit rawReceived(units.at(i), source);
        for (const Message &message : messages.at(i)) {
            emit messageReceived(message, source);
        }
    }
}

void Link::dropStream(Parser *parser)
{
    if (m_streams.contains(parser)) {
        const auto source = dataSource(parser);
        m_streams.remove(parser);
        emit streamClosed(source);
        if (!source.endpoint.isEmpty()) {
            m_endpointRequests.remove(source.endpoint);
            if (auto master = m_endpointMasters.take(source.endpoint)) master->deleteLater();
        }
    }
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
                emit bytesSent(reply.bytes, dataSource(reply.stream));
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
    dropStream(m_parser.get());
    if (m_parser)
        m_parser->reset();
}

} // namespace fhmi
