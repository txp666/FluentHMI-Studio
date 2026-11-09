#include "comm/comm_hub.h"

#include "comm/can/can_link.h"
#include "comm/modbus_slave.h"
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
        const bool outer = m_canText;
        m_canText = link->isCan();
        emit rawReceived(bytes);
        m_canText = outer;
    });
    connect(link, &Link::bytesSent, this, [this, link](const QByteArray &bytes) {
        const bool outer = m_canText;
        m_canText = link->isCan();
        emit bytesSent(bytes);
        m_canText = outer;
    });
    connect(link, &Link::errorOccurred, this, &CommHub::linkError);
    connect(link, &Link::warningMessage, this, &CommHub::linkWarning);
    connect(link, &Link::infoMessage, this, &CommHub::linkInfo);
    if (auto *can = qobject_cast<CanLink *>(link)) {
        connect(can, &CanLink::canFrames, this,
                [this, can](const QList<CanFrame> &frames, bool sent) { emit canFrames(can, frames, sent); });
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

QList<CanLink *> CommHub::connectedCanLinks() const
{
    QList<CanLink *> out;
    for (Link *link : connectedLinks()) {
        if (auto *can = qobject_cast<CanLink *>(link)) {
            out.append(can);
        }
    }
    return out;
}

bool CommHub::hasConnectedByteLink() const
{
    for (Link *link : connectedLinks()) {
        if (!link->isCan()) {
            return true;
        }
    }
    return false;
}

int CommHub::send(const QByteArray &bytes, QList<QByteArray> *sent)
{
    int accepted = 0;
    for (Link *link : connectedLinks()) {
        if (link->isCan()) {
            continue; // bytes mean nothing on a CAN bus
        }
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
        if (link->answersModbus() || link->isCan()) {
            continue; // a slave link puts no Modbus frame on the line, nor does a CAN bus carry one
        }
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

int CommHub::sendCan(const CanCommand &command, QList<QByteArray> *sent, QString *error)
{
    // Every link resolves the command first, so a bad one goes nowhere rather than to some links.
    QList<QPair<CanLink *, CanLink::Prepared>> prepared;
    for (CanLink *link : connectedCanLinks()) {
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

int CommHub::sendCanFrame(const CanFrame &frame, QString *error)
{
    int accepted = 0;
    for (CanLink *link : connectedCanLinks()) {
        accepted += link->sendFrame(frame, error) ? 1 : 0;
    }
    return accepted;
}

int CommHub::send(const EncodedCommand &command, QList<QByteArray> *sent, QString *error)
{
    switch (command.kind) {
    case EncodedCommand::Kind::Modbus:
        return sendModbus(command.modbus, sent);
    case EncodedCommand::Kind::Can:
        return sendCan(command.can, sent, error);
    case EncodedCommand::Kind::Bytes:
        break;
    }
    return send(command.bytes, sent);
}

void CommHub::addModbusSlave(ModbusSlave *slave)
{
    m_slaves.append(slave);
    connect(slave, &ModbusSlave::changed, this, &CommHub::deliverSlaveData);
    connect(slave, &ModbusSlave::logged, this, &CommHub::slaveLog);
    connect(slave, &QObject::destroyed, this, [this]() {
        m_slaves.removeAll(QPointer<ModbusSlave>());
        emit slavesChanged();
    });
    emit slavesChanged();
}

QList<ModbusSlave *> CommHub::modbusSlaves() const
{
    QList<ModbusSlave *> out;
    for (const QPointer<ModbusSlave> &slave : m_slaves) {
        if (slave) {
            out.append(slave.data());
        }
    }
    return out;
}

void CommHub::deliverSlaveStart()
{
    for (ModbusSlave *slave : modbusSlaves()) {
        for (const Message &message : slave->startMessages()) {
            deliverSlaveData(message);
        }
    }
}

void CommHub::deliverSlaveData(const Message &message)
{
    const bool outer = m_slaveData;
    m_slaveData = true;
    emit messageReceived(message);
    m_slaveData = outer;
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
