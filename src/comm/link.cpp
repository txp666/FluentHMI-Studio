#include "comm/link.h"

#include "comm/modbus_master.h"
#include "comm/modbus_slave.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <algorithm>

namespace fhmi {

Link::Link(QObject *parent)
    : QObject(parent), m_parser(createParser(m_parserType, m_parserConfig, nullptr, m_modbusRequests))
{
    qRegisterMetaType<DataSource>();
    connect(this, &Link::connectedChanged, this, [this](bool connected) {
        m_sendSession = nextDataStream();
        if (!connected) {
            for (auto *timer : std::as_const(m_idleTimers))
                timer->stop();
            const auto streams = std::exchange(m_streams, {});
            for (const auto &stream : streams)
                emit streamClosed(dataSource(stream));
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
    auto *key = parser ? parser : m_parser.get();
    auto stream = m_streams.constFind(key);
    if (stream == m_streams.cend())
        return dataSource(*m_streams.insert(key, {nextDataStream(), {}, deviceIdentity()}));
    return dataSource(*stream);
}

DataSource Link::dataSource(const Stream &stream) const
{
    return {m_connectionId, stream.id, stream.endpoint, textEncoding(), isCan(), false, stream.device, parsesModbus()};
}

DataSource Link::eventSource(const QString &endpoint) const
{
    if (!endpoint.isEmpty()) {
        for (const auto &stream : m_streams) {
            if (stream.endpoint == endpoint)
                return {m_connectionId, 0, endpoint, textEncoding(), isCan(), false, stream.device, parsesModbus()};
        }
    }
    return {m_connectionId, 0, endpoint, textEncoding(), isCan(), false};
}

bool Link::ownsStream(quint64 streamId, const QString &endpoint) const
{
    for (const auto &stream : m_streams) {
        if (stream.id == streamId && (endpoint.isEmpty() || endpoint == stream.endpoint))
            return true;
    }
    return false;
}

FrameProgress Link::frameProgress(quint64 streamId) const
{
    for (auto it = m_streams.cbegin(); it != m_streams.cend(); ++it) {
        if (it->id == streamId && it.key())
            return it.key()->frameProgress();
    }
    return {};
}

bool Link::canSendTo(const QString &endpoint) const
{
    return isConnected() && canSend() && (endpoint.isEmpty() || sendEndpoints().contains(endpoint));
}

quint64 Link::sendSession(const QString &endpoint) const
{
    return canSendTo(endpoint) ? m_sendSession : 0;
}

quint64 Link::streamIdForEndpoint(const QString &endpoint) const
{
    for (const auto &stream : m_streams) {
        if (stream.endpoint == endpoint)
            return stream.id;
    }
    return 0;
}

SendStatus Link::sendPeriodic(const QByteArray &bytes, const QString &endpoint)
{
    if (!canSendTo(endpoint))
        return SendStatus::Failed;
    if (hasPendingWrite(endpoint))
        return SendStatus::Busy;
    return send(bytes, endpoint) ? SendStatus::Submitted : SendStatus::Failed;
}

void Link::registerStream(Parser *parser, const QString &endpoint, const QString &device)
{
    dropStream(parser);
    m_streams.insert(parser, Stream{nextDataStream(), endpoint, device});
}

qint64 Link::writeTo(const QByteArray &bytes, const QString &endpoint)
{
    if (!endpoint.isEmpty()) {
        emit errorOccurred(tr("此连接没有可指定的子通道"), endpoint);
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
    const qint64 written = writeTo(bytes, endpoint);
    if (written == WriteFailureReported) {
        return false;
    }
    if (written != bytes.size()) {
        emit errorOccurred(written < 0 ? tr("写入失败") : tr("只写入了 %1/%2 字节").arg(written).arg(bytes.size()),
                           endpoint);
        return false;
    }
    if (parsesModbus()) {
        for (const QString &target : targets)
            requestsFor(target)->noteFrame(bytes, m_parserType);
    }
    for (const auto &target : targets) {
        auto source = dataSource();
        source.endpoint = target;
        for (auto it = m_streams.cbegin(); it != m_streams.cend(); ++it) {
            if (it->endpoint == target) {
                source = dataSource(*it);
                break;
            }
        }
        emit bytesSent(bytes, source);
    }
    return true;
}

std::unique_ptr<Parser> Link::makeParser(const QString &type, const QJsonObject &config, QString *error, const std::shared_ptr<ModbusRequests> &requests) const
{
    if (m_slave && LinkParser::supportsModbusSlave(type)) {
        if (type == QLatin1String("modbus"))
            return std::make_unique<ModbusRtuRequestFramer>();
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
    return m_slave && LinkParser::supportsModbusSlave(m_parserType);
}

std::unique_ptr<Parser> Link::createStreamParser(const QString &endpoint)
{
    return makeParser(m_parserType, m_parserConfig, nullptr, requestsFor(endpoint));
}

void Link::deliver(Parser *parser, const QByteArray &bytes, Boundary boundary)
{
    if (bytes.isEmpty()) {
        return;
    }
    const auto source = dataSource(parser);
    if (!parser) {
        emit wireReceived(bytes, source);
        publishUnit(bytes, {}, source);
        return;
    }
    auto units = parser->frames(bytes);
    if (boundary == Boundary::Message && parser->flushOnMessage())
        units += parser->flush();
    emit wireReceived(bytes, source);
    if (!ownsStream(source.stream))
        return;
    reportFramingState(parser, source);
    if (ownsStream(source.stream))
        deliverUnits(parser, units, boundary);
}

void Link::reportFramingState(Parser *parser, const DataSource &source)
{
    if (const int idle = parser->idleTimeout(); idle > 0) {
        QTimer *&timer = m_idleTimers[parser];
        if (!timer) {
            timer = new QTimer(this);
            timer->setSingleShot(true);
            connect(timer, &QTimer::timeout, this, [this, parser] {
                const auto source = dataSource(parser);
                const auto units = parser->flush();
                reportFramingState(parser, source);
                if (ownsStream(source.stream))
                    deliverUnits(parser, units);
            });
        }
        timer->start(idle);
    } else if (auto *timer = m_idleTimers.value(parser)) {
        timer->stop();
    }
    if (const auto notice = parser->takeFrameNotice())
        emit frameRejected(*notice, source);
}

void Link::deliverUnits(Parser *parser, const QList<QByteArray> &units, Boundary boundary)
{
    const auto source = dataSource(parser);
    if (answersModbus()) {
        // Every request is answered, and its reply queued, before anything is emitted: a slot may drop the
        // stream, and its replies with it. A reply goes out after its request was reported.
        for (const QByteArray &unit : units) {
            answer(parser, unit);
        }
        for (const QByteArray &unit : units) {
            if (!ownsStream(source.stream))
                return;
            publishUnit(unit, {}, source);
        }
        sendNextReply();
        return;
    }
    const QByteArray terminator = boundary == Boundary::Message && parserType() == QLatin1String("line")
        ? lineTerminator(parserConfig()) : QByteArray();
    // Parse before emitting signals, which may disconnect and replace this stream.
    QList<QList<DecodedRecord>> records;
    records.reserve(units.size());
    for (const QByteArray &unit : units) {
        auto parsed = parser->decodeRecords(terminator.isEmpty() || unit.endsWith(terminator) ? unit : unit + terminator);
        for (auto &record : parsed)
            annotateMessage(record.values, source);
        records.append(std::move(parsed));
    }
    for (int i = 0; i < units.size(); ++i) {
        if (!ownsStream(source.stream))
            break;
        publishUnit(units.at(i), records.at(i), source);
    }
}

void Link::publishUnit(const QByteArray &bytes, const QList<DecodedRecord> &records, const DataSource &source)
{
    if (!source.modbus)
        emit rawReceived(bytes, source);
    qsizetype offset = 0;
    for (const auto &record : records) {
        if (!ownsStream(source.stream))
            return;
        if (source.modbus)
            emit rawReceived(record.frame, source);
        if (!ownsStream(source.stream))
            return;
        emit messageReceived(record.values, source);
        if (!ownsStream(source.stream))
            return;
        // A message transport can supply a synthetic line terminator to the decoder.
        const auto end = std::min(record.end, bytes.size());
        emit unitReceived(record.frame.isEmpty() ? bytes.mid(offset, end - offset) : record.frame, record.values, source);
        offset = end;
    }
    if (!source.modbus && offset < bytes.size() && ownsStream(source.stream))
        emit unitReceived(bytes.mid(offset), {}, source);
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
    auto result = std::make_shared<ModbusSlaveAnswer>(m_slave->request(framing, frame, dataSource(parser).endpoint));
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
                const auto source = dataSource(reply.stream);
                emit bytesSent(reply.bytes, source);
                if (m_slave) {
                    m_slave->replied(*reply.answer, source.endpoint);
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
