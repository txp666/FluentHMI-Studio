#include "comm/comm_hub.h"

#include "core/protocol_parser.h"

namespace fhmi {

CommHub::CommHub(QObject *parent) : QObject(parent) {}

QString CommHub::decodeRaw(const QByteArray &bytes) const
{
    return LineParser::decode(bytes, m_rawEncoding);
}

void CommHub::configure(const QJsonObject &projectSerial)
{
    m_projectSerial = projectSerial;
    m_parserType = projectSerial.value(QStringLiteral("parser")).toString(QStringLiteral("line"));
    m_parserConfig = projectSerial.value(QStringLiteral("parserConfig")).toObject();
}

void CommHub::addLink(Link *link)
{
    for (const QPointer<Link> &existing : m_links) {
        if (existing == link) {
            return;
        }
    }
    m_links.append(link);
    connect(link, &Link::messageReceived, this, &CommHub::messageReceived);
    connect(link, &Link::rawReceived, this, [this, link](const QByteArray &bytes) {
        m_rawEncoding = link->textEncoding();
        emit rawReceived(bytes);
    });
    connect(link, &Link::bytesSent, this, &CommHub::bytesSent);
    connect(link, &Link::errorOccurred, this, &CommHub::linkError);
    connect(link, &Link::infoMessage, this, &CommHub::linkInfo);
    connect(link, &Link::connectedChanged, this, [this, link](bool connected) {
        if (connected) {
            emit linkConnected(link->description());
            emit linkUp(link);
        } else {
            emit linkDisconnected(link->description());
        }
        emit connectionStateChanged(isConnected());
    });
    connect(link, &QObject::destroyed, this, [this]() {
        m_links.removeAll(QPointer<Link>());
        emit connectionStateChanged(isConnected());
    });
}

void CommHub::removeLink(Link *link)
{
    if (!link) {
        return;
    }
    link->disconnect(this);
    m_links.removeAll(QPointer<Link>(link));
    emit connectionStateChanged(isConnected());
}

QList<Link *> CommHub::connectedLinks() const
{
    QList<Link *> out;
    for (const QPointer<Link> &link : m_links) {
        if (link && link->isConnected()) {
            out.append(link.data());
        }
    }
    return out;
}

bool CommHub::isConnected() const
{
    return !connectedLinks().isEmpty();
}

int CommHub::send(const QByteArray &bytes, QList<QByteArray> *sent)
{
    int accepted = 0;
    for (Link *link : connectedLinks()) {
        if (link->send(bytes)) {
            ++accepted;
            if (sent) {
                sent->append(bytes);
            }
        }
    }
    return accepted;
}

int CommHub::sendModbus(const ModbusRequest &request, QList<QByteArray> *sent, const ModbusSend &how)
{
    int accepted = 0;
    for (Link *link : connectedLinks()) {
        QByteArray frame;
        if (link->parsesModbus()) {
            // One request on the line at a time: the master sends it once the link is free.
            frame = link->modbusMaster()->enqueue(request, how.timeoutMs, how.priority, how.done, how.owner);
            if (!frame.isEmpty() && how.queuedOn) {
                how.queuedOn->append(link->modbusMaster());
            }
        } else if (!how.modbusLinksOnly) {
            frame = CommandEncoder::rtuFrame(request);
            if (!link->send(frame)) {
                frame.clear();
            }
        }
        if (!frame.isEmpty()) {
            ++accepted;
            if (sent) {
                sent->append(frame);
            }
        }
    }
    return accepted;
}

int CommHub::send(const EncodedCommand &command, QList<QByteArray> *sent)
{
    return command.kind == EncodedCommand::Kind::Modbus ? sendModbus(command.modbus, sent)
                                                        : send(command.bytes, sent);
}

void CommHub::disconnectAll()
{
    const QList<QPointer<Link>> links = m_links;
    for (const QPointer<Link> &link : links) {
        if (link) {
            link->close();
        }
    }
}

} // namespace fhmi
