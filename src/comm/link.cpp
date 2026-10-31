#include "comm/link.h"

#include "comm/modbus_master.h"

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

bool Link::setParser(const QString &type, const QJsonObject &config, QString *error)
{
    auto parser = createParser(type, config, error, m_modbusRequests);
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
    return m_parserType == QLatin1String("modbus") || m_parserType == QLatin1String("modbus_tcp") ||
           m_parserType == QLatin1String("modbus_ascii");
}

std::unique_ptr<Parser> Link::createStreamParser() const
{
    return createParser(m_parserType, m_parserConfig, nullptr, m_modbusRequests);
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
}

void Link::resetDefaultParser()
{
    if (m_parser) {
        m_parser->reset();
        if (QTimer *timer = m_idleTimers.value(m_parser.get())) {
            timer->stop(); // nothing left to flush
        }
    }
}

} // namespace fhmi
