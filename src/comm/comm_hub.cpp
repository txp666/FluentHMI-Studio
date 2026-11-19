#include "comm/comm_hub.h"

#include "comm/can/can_link.h"
#include "comm/modbus_slave.h"
#include "comm/serial_link.h"
#include "core/protocol_parser.h"

#include <QtCore/QUuid>
#include <algorithm>

namespace fhmi {

QString SendTarget::label() const
{
    return link->connectionId() + QStringLiteral(" · ") + (endpoint.isEmpty() ? link->description() : endpoint);
}

CommHub::CommHub(QObject *parent) : QObject(parent), m_virtualSerialPair(new VirtualSerialPair(this)) {}

QString CommHub::mqttTopicSession()
{
    if (m_mqttTopicSession.isEmpty())
        m_mqttTopicSession = QUuid::createUuid().toString(QUuid::Id128).left(16);
    return m_mqttTopicSession;
}

QString CommHub::decodeRaw(const QByteArray &bytes, const DataSource &source) const
{
    return LineParser::decode(bytes, source.encoding);
}

Link *CommHub::sourceLink(const DataSource &source) const
{
    for (Link *link : m_links) {
        if (link && link->connectionId() == source.connectionId && link->ownsStream(source.stream, source.endpoint))
            return link;
    }
    return nullptr;
}

void CommHub::configure(const QJsonObject &projectSerial)
{
    m_mqttTopicSession.clear();
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
    connect(link, &Link::wireReceived, this, &CommHub::wireReceived);
    connect(link, &Link::unitReceived, this, &CommHub::unitReceived);
    connect(link, &Link::frameRejected, this, &CommHub::frameRejected);
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
    connect(link, &Link::errorOccurred, this, [this, link](const QString &message, const QString &endpoint) {
        emit linkError(message, link->eventSource(endpoint));
    });
    connect(link, &Link::warningMessage, this, [this, link](const QString &message, const QString &endpoint) {
        emit linkWarning(message, link->eventSource(endpoint));
    });
    connect(link, &Link::infoMessage, this, [this, link](const QString &message, const QString &endpoint) {
        emit linkInfo(message, link->eventSource(endpoint));
    });
    if (auto *can = qobject_cast<CanLink *>(link)) {
        connect(can, &CanLink::periodicResult, this, &CommHub::canPeriodicResult);
        connect(can, &CanLink::legacyCyclesChanged, this, &CommHub::canLegacyCyclesChanged);
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
            emit linkConnected(link->description(), link->eventSource());
        } else {
            emit linkDisconnected(link->description(), link->eventSource());
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

QList<SendTarget> CommHub::sendTargets(const EncodedCommand &command) const
{
    using Kind = EncodedCommand::Kind;
    QList<SendTarget> targets;
    if (command.kind == Kind::S7)
        return targets;
    const bool can = command.kind == Kind::Can || command.kind == Kind::CanFrame;
    for (auto *link : connectedLinks(command.route)) {
        if (link->isCan() != can || (can && !command.route.endpoint.isEmpty()) ||
            (command.kind == Kind::Modbus && link->answersModbus()))
            continue;
        const auto endpoints = command.route.endpoint.isEmpty() ? link->sendEndpoints() : QStringList{command.route.endpoint};
        for (const auto &endpoint : endpoints) {
            if (const auto session = link->sendSession(endpoint); session)
                targets.append({link, endpoint, session});
        }
    }
    std::sort(targets.begin(), targets.end(), [](const SendTarget &a, const SendTarget &b) {
        return a.session == b.session ? a.endpoint < b.endpoint : a.session < b.session;
    });
    return targets;
}

int CommHub::send(const QByteArray &bytes, QList<QByteArray> *sent, const DataRoute &route)
{
    EncodedCommand command;
    command.bytes = bytes;
    command.route = route;
    return send(command, sendTargets(command), sent);
}

int CommHub::sendModbus(const ModbusRequest &request, QList<QByteArray> *sent, const DataRoute &route)
{
    EncodedCommand command;
    command.kind = EncodedCommand::Kind::Modbus;
    command.modbus = request;
    command.route = route;
    return send(command, sendTargets(command), sent);
}

QByteArray CommHub::sendModbusTo(const ModbusRequest &request, const SendTarget &target)
{
    auto *link = target.link.data();
    if (!link->parsesModbus()) {
        const auto frame = CommandEncoder::rtuFrame(request);
        return link->send(frame, target.endpoint) ? frame : QByteArray{};
    }
    auto *master = link->modbusMaster(target.endpoint);
    const auto source = link->eventSource(target.endpoint);
    const QPointer<CommHub> self(this);
    return master->enqueue(request, 1000, ModbusMaster::Priority::Write, [self, source](const ModbusResult &result) {
        if (!self)
            return;
        if (result.ok)
            emit self->linkInfo(tr("已收到 Modbus 设备应答"), source);
        else
            emit self->linkError(result.problem, source);
    }, target.endpoint);
}

int CommHub::sendCan(const CanCommand &can, QList<QByteArray> *sent, QString *error, const DataRoute &route)
{
    EncodedCommand command;
    command.kind = EncodedCommand::Kind::Can;
    command.can = can;
    command.route = route;
    return send(command, sendTargets(command), sent, error);
}

int CommHub::sendCanFrame(const CanFrame &frame, QString *error, const DataRoute &route)
{
    EncodedCommand command;
    command.kind = EncodedCommand::Kind::CanFrame;
    command.frame = frame;
    command.route = route;
    return send(command, sendTargets(command), nullptr, error);
}

int CommHub::sendCanTo(const EncodedCommand &command, const QList<SendTarget> &targets, QList<QByteArray> *sent,
                       QString *error)
{
    QList<CanLink::Prepared> prepared;
    prepared.reserve(targets.size());
    for (const auto &target : targets) {
        auto *link = static_cast<CanLink *>(target.link.data());
        QString problem;
        const auto frame = command.kind == EncodedCommand::Kind::CanFrame
            ? link->prepareRawFrame(command.frame, &problem) : link->prepare(command.can, &problem);
        if (!frame || !link->canCommit(*frame, &problem)) {
            if (error)
                *error = tr("%1 · %2").arg(target.label(), problem);
            emit linkError(problem, link->eventSource());
            return 0;
        }
        prepared.append(*frame);
    }

    int accepted = 0;
    for (qsizetype i = 0; i < targets.size(); ++i) {
        const auto &target = targets.at(i);
        if (!target.isCurrent()) {
            if (error)
                *error = tr("发送目标已变化，请重新下发");
            break;
        }
        auto *link = static_cast<CanLink *>(target.link.data());
        const auto &frame = prepared.at(i);
        const auto label = target.label();
        QString problem;
        if (link->commit(frame, &problem)) {
            ++accepted;
            if (sent)
                sent->append(Can::frameLine(frame.frame).toUtf8());
        } else if (error && error->isEmpty()) {
            *error = tr("%1 · %2").arg(label, problem);
        }
    }
    return accepted;
}

int CommHub::send(const EncodedCommand &command, const QList<SendTarget> &targets, QList<QByteArray> *sent,
                  QString *error)
{
    if (!std::all_of(targets.cbegin(), targets.cend(), [](const auto &target) { return target.isCurrent(); })) {
        if (error)
            *error = tr("发送目标已变化，请重新下发");
        return 0;
    }
    using Kind = EncodedCommand::Kind;
    if (command.kind == Kind::S7)
        return 0;
    if (command.kind == Kind::Can || command.kind == Kind::CanFrame)
        return sendCanTo(command, targets, sent, error);

    int accepted = 0;
    for (const auto &target : targets) {
        if (!target.isCurrent()) {
            if (error)
                *error = tr("发送目标已变化，请重新下发");
            break;
        }
        QByteArray frame;
        if (command.kind == Kind::Modbus)
            frame = sendModbusTo(command.modbus, target);
        else if (target.link->send(command.bytes, target.endpoint))
            frame = command.bytes;
        if (!frame.isEmpty()) {
            ++accepted;
            if (sent)
                sent->append(frame);
        }
    }
    return accepted;
}

void CommHub::addModbusSlave(ModbusSlave *slave, const QString &connectionId)
{
    m_slaves.append(slave);
    m_slaveSources.insert(slave, {connectionId, nextDataStream(), {}, QStringLiteral("utf-8"), false, true});
    connect(slave, &ModbusSlave::changed, this, [this, slave](const Message &message) { deliverSlaveData(message, slave); });
    connect(slave, &ModbusSlave::logged, this,
            [this, slave](const QString &text, const QString &level, const QString &endpoint) {
                auto source = m_slaveSources.value(slave);
                source.endpoint = endpoint;
                emit slaveLog(text, level, source);
            });
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
    const auto source = m_slaveSources.value(slave);
    emit unitReceived({}, message, source);
    emit messageReceived(message, source);
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
