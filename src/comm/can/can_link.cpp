#include "comm/can/can_link.h"
#include "comm/can/can_diagnostics.h"
#include "comm/can/can_session.h"
#include "comm/can/zlg_timing.h"
#include "core/can_timing.h"

#include "core/dbc.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>

#include <limits>

namespace fhmi {

namespace {

// The same send error again within this time is not reported again: a cyclic frame on a dead bus would flood
// the log otherwise.
constexpr qint64 kRepeatErrorMs = 2000;
constexpr qsizetype kMaxWaitingCommands = 4096;

quint64 frameKey(const CanFrame &frame)
{
    return CanId{frame.id, frame.extended}.key();
}

} // namespace

QString canBusStateText(CanBusState state)
{
    switch (state) {
    case CanBusState::Normal:
        return CanLink::tr("正常");
    case CanBusState::Warning:
        return CanLink::tr("错误警告");
    case CanBusState::Passive:
        return CanLink::tr("错误被动");
    case CanBusState::BusOff:
        return CanLink::tr("总线关闭");
    }
    return QString();
}

CanLink::CanLink(QObject *parent) : Link(parent)
{
    qRegisterMetaType<fhmi::CanFrame>();
    qRegisterMetaType<QList<fhmi::CanFrame>>();
    qRegisterMetaType<fhmi::CanBusState>();
}

CanLink::~CanLink()
{
    if (m_backend) {
        m_backend->disconnect(this);
        m_backend->close();
    }
    dropTable();
}

std::unique_ptr<CanBackend> CanLink::createBackend(const QString &adapter, QString *error)
{
    return CanBackend::create(adapter, error);
}

bool CanLink::open(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                   const QString &filter, QString *error)
{
    QString why;
    if (openChannel(settings, dbcPath, inlineSignals, filter, &why)) {
        return true;
    }
    // The reason goes to the log too, as a link's errors do.
    emit errorOccurred(why);
    if (error) {
        *error = why;
    }
    return false;
}

bool CanLink::openAsync(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                        const QString &filter, QString *error)
{
    QString why;
    if (openChannel(settings, dbcPath, inlineSignals, filter, &why, true)) return true;
    if (error) *error = why;
    emit errorOccurred(why);
    return false;
}

bool CanLink::openChannel(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                          const QString &filter, QString *error, bool asynchronous)
{
    if (isBusy()) {
        if (error) *error = tr("CAN 连接操作正在进行中");
        return false;
    }
    close();
    if (m_closing) {
        if (error) *error = tr("请等待 CAN 通道断开后再连接");
        return false;
    }
    std::shared_ptr<const CanDatabase> dbc;
    if (!dbcPath.isEmpty()) {
        auto loaded = Dbc::load(dbcPath, error);
        if (!loaded) {
            return false;
        }
        dbc = std::make_shared<const CanDatabase>(std::move(*loaded));
    }
    auto signalSet = CanSignalSet::build(dbc, inlineSignals, error);
    if (!signalSet) {
        return false;
    }
    auto parsedFilter = CanFilter::parse(filter, error);
    if (!parsedFilter) {
        return false;
    }
    const auto initialState = CanInitialState::build(*signalSet, settings.txInitialState, error);
    if (!initialState || !initialState->acceptsFilter(*parsedFilter, error)) {
        return false;
    }
    if (settings.txInitialMaxAgeMs < 1 || settings.txInitialMaxAgeMs > 60000) {
        if (error) {
            *error = tr("发送初值反馈最大间隔须为 1–60000 ms");
        }
        return false;
    }
    std::unique_ptr<CanBackend> backend = asynchronous ? std::make_unique<CanSession>() : createBackend(settings.adapter, error);
    if (!backend) {
        return false;
    }
    connect(backend.get(), &CanBackend::framesReceived, this, &CanLink::onReceived);
    connect(backend.get(), &CanBackend::framesSent, this, &CanLink::onSent);
    connect(backend.get(), &CanBackend::busStateChanged, this, &CanLink::onBusState);
    connect(backend.get(), &CanBackend::frameSendFailed, this, &CanLink::onFrameSendFailed);
    connect(backend.get(), &CanBackend::sendFailed, this, &CanLink::onSendFailed);
    connect(backend.get(), &CanBackend::failed, this, [this](const QString &message) {
        const QPointer<CanLink> alive(this);
        close();
        if (alive)
            emit errorOccurred(message);
    });
    m_initialState = *initialState;
    m_filter = *parsedFilter;
    auto *session = qobject_cast<CanSession *>(backend.get());
    if (session) {
        connect(session, &CanSession::opened, this, [this](bool success, const QString &why) {
            m_opening = false;
            if (success) opened();
            else {
                const QPointer<CanLink> alive(this);
                close();
                if (alive)
                    emit errorOccurred(why);
            }
        });
        connect(session, &CanSession::closed, this, [this, session] {
            m_backend = nullptr;
            m_opening = m_closing = false;
            session->deleteLater();
            emit lifecycleChanged();
        });
    }
    if (!backend->open(settings, error)) return false;
    m_backend = backend.release();
    m_backend->setParent(this);
    m_signals = std::make_shared<const CanSignalSet>(std::move(*signalSet));
    m_filter = *parsedFilter;
    m_settings = settings;
    m_loadRate = settings.adapter == CanAdapter::SocketCan ? 0 : settings.bitrate;
    m_loadDataRate = settings.dataBitrate;
    if (!settings.customTiming.trimmed().isEmpty()) {
        m_loadRate = 0;
        if (settings.adapter == CanAdapter::Pcan) {
            if (auto custom = CanBitTiming::parseCustom(settings.customTiming)) {
                m_loadRate = custom->nominal.bitrate(settings.fd ? 80000000 : 8000000);
                if (custom->data)
                    m_loadDataRate = custom->data->bitrate(80000000);
            }
        } else if (settings.adapter == CanAdapter::Zlg) {
            const auto name = settings.zlgModel.toLatin1();
            if (const auto *model = zlg::findModel(name.constData(), name.size())) {
                if (auto timing = zlg::channelTiming(*model, settings); timing && timing->btr) {
                    const int btr = *timing->btr;
                    m_loadRate = 8000000.0 / (((btr >> 8) & 63) + 1) / (1 + (btr & 15) + 1 + ((btr >> 4) & 7) + 1);
                }
            }
        }
    }
    m_loadPercent.reset();
    m_busySeconds = 0;
    m_received = 0;
    m_sent = 0;
    m_state = CanBusState::Normal;
    m_lastSendError.clear();
    if (session) {
        m_opening = true;
        emit lifecycleChanged();
    } else opened();
    return true;
}

void CanLink::opened()
{
    m_loadClock.start();
    const QPointer<CanLink> alive(this);
    const quint64 generation = m_generation;
    emit lifecycleChanged();
    if (!alive || m_generation != generation)
        return;
    emit connectedChanged(true);
    if (!alive || m_generation != generation)
        return;
    const QStringList warnings = m_signals->warnings();
    for (const QString &warning : warnings) {
        if (!alive || m_generation != generation)
            return;
        emit warningMessage(warning);
    }
}

void CanLink::close()
{
    dropTable();
    if (!m_backend) {
        return;
    }
    if (auto *session = qobject_cast<CanSession *>(m_backend)) {
        if (m_closing) return;
        const bool connected = isConnected();
        m_opening = false;
        m_closing = true;
        session->close();
        const QPointer<CanLink> alive(this);
        if (connected) emit connectedChanged(false);
        if (alive)
            emit lifecycleChanged();
        return;
    }
    CanBackend *backend = m_backend;
    m_backend = nullptr;
    backend->disconnect(this);
    backend->close();
    backend->deleteLater(); // this may run inside one of its signals
    emit connectedChanged(false);
}

void CanLink::dropTable()
{
    ++m_generation;
    for (const Entry &entry : std::as_const(m_table)) {
        if (entry.timer) {
            entry.timer->stop();
            entry.timer->deleteLater();
        }
    }
    m_table.clear();
    m_waiting = 0;
    m_initialState.clearReceived();
}

QString CanLink::description() const
{
    return m_backend ? m_backend->description() : tr("CAN 未连接");
}

bool CanLink::isConnected() const
{
    return m_backend && !isBusy() && m_backend->isOpen();
}

qint64 CanLink::writeBytes(const QByteArray &)
{
    return WriteFailureReported; // bytes have no meaning on a CAN bus; the hub sends none here
}

std::optional<CanLink::Prepared> CanLink::prepare(const CanCommand &command, QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (!m_signals) {
        return fail(tr("CAN 未连接"));
    }
    const auto target = m_signals->target(command.target, error);
    if (!target) {
        return std::nullopt;
    }
    Prepared prepared;
    prepared.connectionGeneration = m_generation;
    CanFrame &frame = prepared.frame;
    frame.id = target->id.id;
    frame.extended = target->id.extended;
    const QString shownName = target->name.isEmpty() ? Can::idText(target->id) : target->name;
    if (target->hasSignals) {
        const auto assignments = parseAssignments(command.assignments, error);
        if (!assignments) {
            return std::nullopt;
        }
        // A DBC message is FD as the DBC says; an ID with inline signals only as the command (or its length) says.
        frame.fd = target->fd || (target->name.isEmpty() && command.fd);
        frame.requireAdapterReply = m_initialState.hasRule(target->id);
        prepared.initialData = QByteArray(target->length, '\0');
        for (const CanSignalDef *signal : target->signalDefs) {
            if (signal->fits(target->length))
                signal->putRawBits(prepared.initialData, signal->initialBits());
        }
        prepared.signalMask = QByteArray(target->length, '\0');
        frame.data = QByteArray(target->length, '\0');
        for (const auto &[name, value] : *assignments) {
            const CanSignalDef *signal = nullptr;
            for (const CanSignalDef *candidate : target->signalDefs) {
                signal = candidate->name == name ? candidate : signal;
            }
            if (!signal) {
                return fail(tr("报文 %1 没有信号 %2").arg(shownName, name));
            }
            if (!signal->fits(target->length)) {
                return fail(tr("信号 %1 超出报文 %2 的 %3 字节").arg(name, shownName).arg(target->length));
            }
            const auto bits = signal->encode(value, error);
            if (!bits) {
                return std::nullopt;
            }
            signal->putRawBits(frame.data, *bits);
            signal->putRawBits(prepared.signalMask, std::numeric_limits<quint64>::max());
        }
    } else {
        if (!command.payload) {
            return fail(command.payloadError);
        }
        frame.fd = command.fd;
        frame.data = *command.payload;
        if (frame.fd ? !Can::padFdPayload(frame.data) : frame.data.size() > 8) {
            return fail(frame.fd ? tr("CAN FD 数据最多 64 字节")
                                 : tr("CAN 数据最多 8 字节，更长的报文请勾选 canFd 用 CAN FD 发送"));
        }
    }
    if (frame.fd && !m_settings.fd) {
        return fail(tr("报文 %1 是 CAN FD 报文，请在 CAN 卡片上勾选 CAN FD 后重新连接").arg(shownName));
    }
    frame.brs = frame.fd && m_settings.brs;
    prepared.periodMs = command.cycleMs > 0 ? command.cycleMs : target->cycleTime;
    const auto composed = composeFrame(prepared, error);
    if (!composed)
        return std::nullopt;
    prepared.frame = *composed;
    return prepared;
}

std::optional<CanFrame> CanLink::composeFrame(const Prepared &prepared, QString *error) const
{
    CanFrame frame = prepared.frame;
    if (prepared.signalMask.isEmpty())
        return frame;

    const auto existing = m_table.constFind(frameKey(frame));
    if (existing != m_table.constEnd() && existing->frame &&
        existing->frame->data.size() == prepared.initialData.size()) {
        frame.data = existing->frame->data;
    } else {
        frame.data = prepared.initialData;
        if (!m_initialState.initialize({frame.id, frame.extended}, frame.data, Can::monotonicMs(),
                                       m_settings.txInitialMaxAgeMs, error)) {
            return std::nullopt;
        }
    }
    for (qsizetype i = 0; i < frame.data.size(); ++i) {
        const quint8 mask = quint8(prepared.signalMask.at(i));
        frame.data[i] = char((quint8(frame.data.at(i)) & ~mask) |
                             (quint8(prepared.frame.data.at(i)) & mask));
    }
    return frame;
}

bool CanLink::checkSendAvailable(QString *error) const
{
    if (!isConnected() || m_backend->transmitState()->stopping) {
        if (error)
            *error = tr("CAN 未连接");
        return false;
    }
    if (m_state == CanBusState::BusOff || m_backend->transmitState()->busOff) {
        if (error)
            *error = CanBackend::busOffSendError();
        return false;
    }
    return true;
}

bool CanLink::commit(const Prepared &prepared, QString *error)
{
    if (prepared.connectionGeneration != m_generation) {
        if (error)
            *error = tr("CAN 未连接");
        return false;
    }
    if (!checkSendAvailable(error))
        return false;
    const quint64 key = frameKey(prepared.frame);
    const auto entry = m_table.find(key);
    if (entry != m_table.end() && (entry->submission.id || !entry->queue.isEmpty())) {
        if (m_waiting >= kMaxWaitingCommands) {
            if (error)
                *error = CanBackend::tr("CAN 发送队列已满");
            return false;
        }
        entry->queue.enqueue({prepared, m_backend->transmitState()->generation});
        ++m_waiting;
        return true;
    }
    const auto frame = composeFrame(prepared, error);
    return frame && submitFrame(*frame, prepared.periodMs, error);
}

bool CanLink::submitFrame(CanFrame frame, std::optional<int> periodMs, QString *error)
{
    frame.submissionId = ++m_nextSubmission;
    m_table[frameKey(frame)].submission = {frame.submissionId, periodMs};
    const QPointer<CanLink> alive(this);
    const bool accepted = sendFrame(frame, error);
    if (!accepted && alive)
        finishSubmission(frame, false);
    return accepted;
}

void CanLink::finishSubmission(const CanFrame &frame, bool accepted)
{
    if (!frame.submissionId)
        return;
    const quint64 key = frameKey(frame);
    auto entry = m_table.find(key);
    if (entry == m_table.end() || entry->submission.id != frame.submissionId)
        return;
    const auto period = entry->submission.periodMs;
    entry->submission = {};
    if (accepted && period) {
        entry->frame = frame;
        if (*period > 0) {
            if (!entry->timer) {
                entry->timer = new QTimer(this);
                entry->timer->setTimerType(Qt::PreciseTimer);
                connect(entry->timer, &QTimer::timeout, this, [this, key] { resend(key); });
            }
            if (entry->timer->interval() != *period || !entry->timer->isActive())
                entry->timer->start(*period);
        }
    }
    if (entry->queue.isEmpty()) {
        if (!entry->frame)
            m_table.erase(entry);
        return;
    }
    const auto generation = m_generation;
    QMetaObject::invokeMethod(this, [this, key, generation] {
        if (m_generation == generation)
            dispatchNext(key);
    }, Qt::QueuedConnection);
}

void CanLink::dispatchNext(quint64 key)
{
    const QPointer<CanLink> alive(this);
    const auto generation = m_generation;
    while (isConnected()) {
        auto entry = m_table.find(key);
        if (entry == m_table.end() || entry->submission.id)
            return;
        if (entry->queue.isEmpty()) {
            if (!entry->frame)
                m_table.erase(entry);
            return;
        }
        const auto next = entry->queue.dequeue();
        --m_waiting;
        if (next.generation != m_backend->transmitState()->generation)
            continue;
        QString error;
        const auto frame = composeFrame(next.command, &error);
        if (frame) {
            if (!submitFrame(*frame, next.command.periodMs, &error) && alive && m_generation == generation)
                onSendFailed(error);
            return; // the adapter result schedules the next command
        }
        onSendFailed(error);
        if (!alive || m_generation != generation)
            return;
    }
}

bool CanLink::sendFrame(const CanFrame &frame, QString *error)
{
    if (!checkSendAvailable(error))
        return false;
    if (frame.fd && !m_settings.fd) {
        if (error) {
            *error = tr("通道是经典 CAN，不能发送 CAN FD 报文（勾选 CAN FD 后重新连接）");
        }
        return false;
    }
    return m_backend->send(frame, error);
}

void CanLink::resend(quint64 key)
{
    const auto it = m_table.constFind(key);
    if (it == m_table.constEnd() || !it->frame || it->submission.id || !it->queue.isEmpty() ||
        !checkSendAvailable(nullptr)) {
        return;
    }
    QString error;
    const QPointer<CanLink> alive(this);
    if (!submitFrame(*it->frame, std::nullopt, &error) && alive) {
        onSendFailed(error);
    }
}

std::optional<double> CanLink::busLoadPercent()
{
    if (!isConnected() || m_loadRate <= 0 || !m_loadClock.isValid())
        return std::nullopt;
    const qint64 elapsed = m_loadClock.elapsed();
    if (elapsed >= 1000) {
        m_loadPercent = std::clamp(m_busySeconds * 100000.0 / elapsed, 0.0, 100.0);
        m_busySeconds = 0;
        m_loadClock.restart();
    }
    return m_loadPercent;
}
void CanLink::recordLoad(const QList<CanFrame> &frames)
{
    busLoadPercent();
    for (const auto &frame : frames)
        m_busySeconds += Can::estimatedFrameSeconds(frame, m_loadRate, m_loadDataRate);
}
void CanLink::onReceived(const QList<CanFrame> &frames)
{
    if (!isConnected()) return;
    if constexpr (CanDiagnostics::BuildEnabled) {
        if (!frames.isEmpty()) {
            CanDiagnostics::record("gui_delivery", {{"link", QString::number(quintptr(this), 16)},
                                                    {"connection_id", dataSource().connectionId},
                                                    {"channel", description()},
                                                    {"received", frames.size()},
                                                    {"oldest_software_timestamp_ms", frames.front().timestampMs},
                                                    {"oldest_queue_age_ms", Can::monotonicMs() - frames.front().receivedAtMs}});
        }
    }
    recordLoad(frames);
    QList<CanFrame> accepted;
    accepted.reserve(frames.size());
    for (const CanFrame &frame : frames) {
        if (!frame.error && m_filter.accepts(frame.id, frame.extended)) {
            m_initialState.observe(frame, frame.receivedAtMs);
            accepted.append(frame);
        }
    }
    if (accepted.isEmpty()) {
        return;
    }
    m_received += quint64(accepted.size());
    const QPointer<CanLink> alive(this);
    const quint64 generation = m_generation;
    const std::shared_ptr<const CanSignalSet> signalSet = m_signals; // a slot may close the link
    emit canFrames(accepted, false);
    for (const CanFrame &frame : std::as_const(accepted)) {
        if (!alive || m_generation != generation) {
            return;
        }
        const Message message = signalSet ? signalSet->decode(frame) : Message{};
        emit frameDecoded(frame, message);
        if (!alive || m_generation != generation)
            return;
        emit rawReceived(Can::frameLine(frame).toUtf8());
        if (!alive || m_generation != generation)
            return;
        if (!message.isEmpty())
            emit messageReceived(message);
    }
}

void CanLink::onSent(const QList<CanFrame> &frames)
{
    if (!isConnected()) return;
    recordLoad(frames);
    m_sent += quint64(frames.size());
    for (const CanFrame &frame : frames)
        finishSubmission(frame, true);
    const QPointer<CanLink> alive(this);
    const quint64 generation = m_generation;
    emit canFrames(frames, true);
    for (const CanFrame &frame : frames) {
        if (!alive || m_generation != generation) {
            return;
        }
        emit bytesSent(Can::frameLine(frame).toUtf8());
    }
}

void CanLink::onBusState(CanBusState state)
{
    if (!isConnected() || state == m_state) {
        return;
    }
    m_state = state;
    const QString text = tr("CAN 总线状态: %1（%2）").arg(canBusStateText(state), description());
    const QPointer<CanLink> alive(this);
    const quint64 generation = m_generation;
    if (state == CanBusState::BusOff) {
        if (m_settings.autoRecover)
            emit warningMessage(text + tr("，自动恢复已启用"));
        else
            emit errorOccurred(text + tr("，断开后重新连接可恢复"));
    } else if (state == CanBusState::Normal) {
        emit infoMessage(text);
    } else {
        emit warningMessage(text);
    }
    if (alive && m_generation == generation)
        emit busStateChanged(state);
}

void CanLink::onFrameSendFailed(const CanFrame &frame, const QString &message)
{
    if (!isConnected())
        return;
    finishSubmission(frame, false);
    onSendFailed(message);
}

void CanLink::onSendFailed(const QString &message)
{
    const qint64 now = Can::monotonicMs();
    if (message == m_lastSendError && now - m_lastSendErrorAt < kRepeatErrorMs) {
        return;
    }
    m_lastSendError = message;
    m_lastSendErrorAt = now;
    emit errorOccurred(message);
}

} // namespace fhmi
