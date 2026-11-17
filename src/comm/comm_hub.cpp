#include "comm/comm_hub.h"

#include "comm/can/can_link.h"
#include "comm/modbus_slave.h"
#include "core/protocol_parser.h"

namespace fhmi {

CommHub::CommHub(QObject *parent) : QObject(parent) {}

QString CommHub::decodeRaw(const QByteArray &bytes, const DataSource &source) const
{
    return LineParser::decode(bytes, source.encoding);
}

void CommHub::configure(const QJsonObject &projectSerial)
{
    m_projectSerial = projectSerial;
    m_parserType = projectSerial.value(QStringLiteral("parser")).toString(QStringLiteral("line"));
    m_parserConfig = projectSerial.value(QStringLiteral("parserConfig")).toObject();
}

void CommHub::addLink(Link *link, const QString &connectionId)
{
    for (const QPointer<Link> &existing : m_links) {
        if (existing == link) {
            return;
        }
    }
    if (!connectionId.isEmpty()) link->setConnectionId(connectionId);
    m_links.append(link);
    connect(link, &Link::streamClosed, this, &CommHub::streamClosed);
    connect(link, &Link::messageReceived, this, [this, link](const Message &message, DataSource source) {
        if (!source.stream) source = link->dataSource();
        emit messageReceived(message, source);
    });
    connect(link, &Link::rawReceived, this, [this, link](const QByteArray &bytes, DataSource source) {
        if (!source.stream) source = link->dataSource();
        emit rawReceived(bytes, source);
    });
    connect(link, &Link::bytesSent, this, [this, link](const QByteArray &bytes, DataSource source) {
        if (!source.stream) source = link->dataSource();
        emit bytesSent(bytes, source);
    });
    connect(link, &Link::errorOccurred, this, &CommHub::linkError);
    connect(link, &Link::warningMessage, this, &CommHub::linkWarning);
    connect(link, &Link::infoMessage, this, &CommHub::linkInfo);
    if (auto *can = qobject_cast<CanLink *>(link)) {
        connect(can, &CanLink::canFrames, this,
                [this, can](const QList<CanFrame> &frames, bool sent) { emit canFrames(can, frames, sent, can->dataSource()); });
        connect(can, &CanLink::frameDecoded, this, [this, can](const CanFrame &frame, const Message &message) {
            emit canFrameDecoded(frame, message, can->dataSource());
        });
        connect(can, &Link::connectedChanged, this, [this, can](bool connected) {
            if (connected) {
                emit canLinkConnected(can);
            }
        });
    }
    connect(link, &Link::connectedChanged, this, [this, link](bool connected) {
        if (connected) {
            emit linkConnected(link->description());
        } else {
            emit linkDisconnected(link->description());
        }
        emit connectionStateChanged(isConnected());
        emit linksChanged();
    });
    connect(link, &Link::canSendChanged, this, &CommHub::linksChanged);
    connect(link, &QObject::destroyed, this, [this]() {
        m_links.removeAll(QPointer<Link>());
        emit connectionStateChanged(isConnected());
        emit linksChanged();
    });
    emit linksChanged();
}

void CommHub::removeLink(Link *link)
{
    if (!link) {
        return;
    }
    link->disconnect(this);
    m_links.removeAll(QPointer<Link>(link));
    emit connectionStateChanged(isConnected());
    emit linksChanged();
}

QList<Link *> CommHub::connectedLinks(const DataRoute &route) const
{
    QList<Link *> out;
    for (const QPointer<Link> &link : m_links) {
        if (link && link->isConnected() && route.acceptsConnection(link->connectionId())) {
            out.append(link.data());
        }
    }
    return out;
}

bool CommHub::isConnected() const
{
    return !connectedLinks().isEmpty();
}

QList<CanLink *> CommHub::connectedCanLinks(const DataRoute &route) const
{
    QList<CanLink *> out;
    if (!route.endpoint.isEmpty()) return out;
    for (Link *link : connectedLinks(route)) {
        if (auto *can = qobject_cast<CanLink *>(link)) {
            out.append(can);
        }
    }
    return out;
}

int CommHub::send(const QByteArray &bytes, QList<QByteArray> *sent, const DataRoute &route)
{
    int accepted = 0;
    for (Link *link : connectedLinks(route)) {
        if (link->isCan() || (!route.endpoint.isEmpty() && !link->canSendTo(route.endpoint))) {
            continue; // bytes mean nothing on a CAN bus
        }
        if (link->send(bytes, route.endpoint)) {
            ++accepted;
            if (sent) {
                sent->append(bytes);
            }
        }
    }
    return accepted;
}

int CommHub::sendModbus(const ModbusRequest &request, QList<QByteArray> *sent, const DataRoute &route)
{
    int accepted = 0;
    for (Link *link : connectedLinks(route)) {
        if (link->answersModbus() || link->isCan()) continue;
        const QStringList endpoints = route.endpoint.isEmpty() ? link->sendEndpoints() : QStringList{route.endpoint};
        for (const QString &endpoint : endpoints) {
            QByteArray frame;
            if (link->parsesModbus()) {
                if (!link->canSendTo(endpoint)) continue;
                auto *master = link->modbusMaster(endpoint);
                frame = master->enqueue(request, 1000, ModbusMaster::Priority::Write, {}, endpoint);
            } else {
                frame = CommandEncoder::rtuFrame(request);
                if (!link->send(frame, endpoint)) frame.clear();
            }
            if (!frame.isEmpty()) {
                ++accepted;
                if (sent) sent->append(frame);
            }
        }
    }
    return accepted;
}

int CommHub::sendCan(const CanCommand &command, QList<QByteArray> *sent, QString *error, const DataRoute &route)
{
    // Every link resolves the command first, so a bad one goes nowhere rather than to some links.
    QList<QPair<CanLink *, CanLink::Prepared>> prepared;
    for (CanLink *link : connectedCanLinks(route)) {
        QString why;
        const auto frame = link->prepare(command, &why);
        if (!frame) {
            emit linkError(why); // the log line; the caller shows the tip
            if (error) {
                *error = why;
            }
            return 0;
        }
        prepared.append({link, *frame});
    }
    int accepted = 0;
    for (const auto &[link, frame] : std::as_const(prepared)) {
        QString why;
        if (link->commit(frame, &why)) {
            ++accepted;
            if (sent) {
                sent->append(Can::frameLine(frame.frame).toUtf8());
            }
        } else if (error) {
            *error = why;
        }
    }
    return accepted;
}

int CommHub::sendCanFrame(const CanFrame &frame, QString *error, const DataRoute &route)
{
    int accepted = 0;
    for (CanLink *link : connectedCanLinks(route)) {
        accepted += link->sendFrame(frame, error) ? 1 : 0;
    }
    return accepted;
}

int CommHub::send(const EncodedCommand &command, QList<QByteArray> *sent, QString *error)
{
    switch (command.kind) {
    case EncodedCommand::Kind::S7:
        return 0; // Addressed PLC operations never enter byte/Modbus broadcasts.
    case EncodedCommand::Kind::Modbus:
        return sendModbus(command.modbus, sent, command.route);
    case EncodedCommand::Kind::Can:
        return sendCan(command.can, sent, error, command.route);
    case EncodedCommand::Kind::Bytes:
        break;
    }
    return send(command.bytes, sent, command.route);
}

void CommHub::addModbusSlave(ModbusSlave *slave, const QString &connectionId)
{
    m_slaves.append(slave);
    m_slaveSources.insert(slave, {connectionId, nextDataStream(), {}, QStringLiteral("utf-8"), false, true});
    connect(slave, &ModbusSlave::changed, this, [this, slave](const Message &message) { deliverSlaveData(message, slave); });
    connect(slave, &ModbusSlave::logged, this, &CommHub::slaveLog);
    connect(slave, &QObject::destroyed, this, [this, slave]() {
        emit streamClosed(m_slaveSources.take(slave));
        m_slaves.removeAll(QPointer<ModbusSlave>());
        emit slavesChanged();
    });
    emit slavesChanged();
}

QList<ModbusSlave *> CommHub::modbusSlaves(const DataRoute &route) const
{
    QList<ModbusSlave *> out;
    for (const QPointer<ModbusSlave> &slave : m_slaves) {
        if (slave && route.accepts(m_slaveSources.value(slave))) {
            out.append(slave.data());
        }
    }
    return out;
}

void CommHub::deliverSlaveStart()
{
    for (ModbusSlave *slave : modbusSlaves()) {
        for (const Message &message : slave->startMessages()) {
            deliverSlaveData(message, slave);
        }
    }
}

void CommHub::deliverSlaveData(const Message &message, ModbusSlave *slave)
{
    emit messageReceived(message, m_slaveSources.value(slave));
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
