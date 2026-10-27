#include "comm/link.h"

namespace fhmi {

Link::Link(QObject *parent) : QObject(parent), m_parser(createParser(m_parserType, m_parserConfig)) {}

Link::~Link() = default;

bool Link::send(const QByteArray &bytes)
{
    if (!canSend() || bytes.isEmpty()) {
        return false;
    }
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
    auto parser = createParser(type, config, error);
    if (!parser) {
        return false;
    }
    m_parserType = type;
    m_parserConfig = config;
    m_parser = std::move(parser);
    return true;
}

std::unique_ptr<Parser> Link::createStreamParser() const
{
    return createParser(m_parserType, m_parserConfig);
}

void Link::deliver(Parser *parser, const QByteArray &bytes)
{
    if (bytes.isEmpty()) {
        return;
    }
    emit rawReceived(bytes);
    if (!parser) {
        return;
    }
    for (const Message &message : parser->feed(bytes)) {
        emit messageReceived(message);
    }
}

void Link::resetDefaultParser()
{
    if (m_parser) {
        m_parser->reset();
    }
}

} // namespace fhmi
