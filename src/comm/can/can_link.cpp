#include "comm/can/can_link.h"

#include "core/dbc.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>

namespace fhmi {

namespace {

// The same send error again within this time is not reported again: a cyclic frame on a dead bus would flood
// the log otherwise.
constexpr qint64 kRepeatErrorMs = 2000;

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

bool CanLink::openChannel(const CanSettings &settings, const QString &dbcPath, const QString &inlineSignals,
                          const QString &filter, QString *error)
{
    close();
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
    std::unique_ptr<CanBackend> backend = createBackend(settings.adapter, error);
    if (!backend) {
        return false;
    }
    connect(backend.get(), &CanBackend::framesReceived, this, &CanLink::onReceived);
    connect(backend.get(), &CanBackend::framesSent, this, &CanLink::onSent);
    connect(backend.get(), &CanBackend::busStateChanged, this, &CanLink::onBusState);
    connect(backend.get(), &CanBackend::sendFailed, this, &CanLink::onSendFailed);
    connect(backend.get(), &CanBackend::failed, this, [this](const QString &message) {
        close();
        emit errorOccurred(message);
    });
    m_initialState = *initialState;
    m_receiveClock.start();
    m_filter = *parsedFilter;
    if (!backend->open(settings, error)) {
        return false;
    }
    m_backend = backend.release();
    m_backend->setParent(this);
    m_signals = std::make_shared<const CanSignalSet>(std::move(*signalSet));
    m_filter = *parsedFilter;
    m_settings = settings;
    m_received = 0;
    m_sent = 0;
    m_state = CanBusState::Normal;
    m_lastSendError.clear();
    emit connectedChanged(true);
    for (const QString &warning : m_signals->warnings()) {
        emit warningMessage(warning);
    }
    return true;
}

void CanLink::close()
{
    dropTable();
    if (!m_backend) {
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
    for (const Entry &entry : std::as_const(m_table)) {
        if (entry.timer) {
            entry.timer->stop();
            entry.timer->deleteLater();
        }
    }
    m_table.clear();
    m_initialState.clearReceived();
    m_receiveClock.invalidate();
}

QString CanLink::description() const
{
    return m_backend ? m_backend->description() : tr("CAN 未连接");
}

bool CanLink::isConnected() const
{
    return m_backend && m_backend->isOpen();
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
        const auto existing = m_table.constFind(target->id.key());
        if (existing != m_table.constEnd() && existing->frame.data.size() == target->length) {
            frame.data = existing->frame.data;
        } else {
            // A message not sent yet starts from each signal's GenSigStartValue, or 0.
            frame.data = QByteArray(target->length, '\0');
            for (const CanSignalDef *signal : target->signalDefs) {
                if (signal->fits(target->length)) {
                    signal->putRawBits(frame.data, signal->initialBits());
                }
            }
            if (!m_initialState.initialize(target->id, frame.data, m_receiveClock.isValid() ? m_receiveClock.elapsed() : 0,
                                           m_settings.txInitialMaxAgeMs, error)) {
                return std::nullopt;
            }
        }
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
    return prepared;
}

bool CanLink::commit(const Prepared &prepared, QString *error)
{
    if (!m_backend) {
        if (error) {
            *error = tr("CAN 未连接");
        }
        return false;
    }
    const quint64 key = CanId{prepared.frame.id, prepared.frame.extended}.key();
    const bool hadEntry = m_table.contains(key);
    const CanFrame previous = hadEntry ? m_table.value(key).frame : CanFrame{};
    const quint64 previousRevision = hadEntry ? m_table.value(key).revision : 0;
    const quint64 revision = ++m_writeRevision;
    m_table[key].frame = prepared.frame;
    m_table[key].revision = revision;
    const QPointer<CanLink> alive(this);
    const bool accepted = sendFrame(prepared.frame, error);
    if (!alive) {
        return accepted;
    }
    if (!m_table.contains(key) || m_table.value(key).revision != revision) {
        return accepted; // a synchronous handler closed the link or issued a newer command
    }
    if (!accepted) {
        if (hadEntry && m_table.contains(key)) {
            m_table[key].frame = previous;
            m_table[key].revision = previousRevision;
        } else {
            m_table.remove(key);
        }
        return false;
    }
    if (!m_backend || !m_table.contains(key)) {
        return true; // a sent-frame handler closed the link; do not restart its cyclic timer
    }
    Entry &entry = m_table[key];
    if (prepared.periodMs > 0) {
        if (!entry.timer) {
            entry.timer = new QTimer(this);
            entry.timer->setTimerType(Qt::PreciseTimer);
            connect(entry.timer, &QTimer::timeout, this, [this, key]() { resend(key); });
        }
        // A new value goes out now and keeps the period; only a new period restarts it.
        if (!entry.timer->isActive() || entry.timer->interval() != prepared.periodMs) {
            entry.timer->start(prepared.periodMs);
        }
    }
    return true;
}

bool CanLink::sendFrame(const CanFrame &frame, QString *error)
{
    if (!m_backend) {
        if (error) {
            *error = tr("CAN 未连接");
        }
        return false;
    }
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
    if (it == m_table.constEnd() || !m_backend) {
        return;
    }
    QString error;
    if (!m_backend->send(it->frame, &error)) {
        onSendFailed(error);
    }
}

void CanLink::onReceived(const QList<CanFrame> &frames)
{
    QList<CanFrame> accepted;
    accepted.reserve(frames.size());
    for (const CanFrame &frame : frames) {
        if (!frame.error && m_filter.accepts(frame.id, frame.extended)) {
            if (m_receiveClock.isValid()) {
                m_initialState.observe(frame, m_receiveClock.elapsed());
            }
            accepted.append(frame);
        }
    }
    if (accepted.isEmpty()) {
        return;
    }
    m_received += quint64(accepted.size());
    const QPointer<CanLink> alive(this);
    const std::shared_ptr<const CanSignalSet> signalSet = m_signals; // a slot may close the link
    emit canFrames(accepted, false);
    for (const CanFrame &frame : std::as_const(accepted)) {
        if (!alive || !m_backend) {
            return;
        }
        emit rawReceived(Can::frameLine(frame).toUtf8());
        if (signalSet) {
            const Message message = signalSet->decode(frame);
            if (!message.isEmpty() && alive) {
                emit messageReceived(message);
            }
        }
    }
}

void CanLink::onSent(const QList<CanFrame> &frames)
{
    m_sent += quint64(frames.size());
    const QPointer<CanLink> alive(this);
    emit canFrames(frames, true);
    for (const CanFrame &frame : frames) {
        if (!alive) {
            return;
        }
        emit bytesSent(Can::frameLine(frame).toUtf8());
    }
}

void CanLink::onBusState(CanBusState state)
{
    if (state == m_state) {
        return;
    }
    m_state = state;
    const QString text = tr("CAN 总线状态: %1（%2）").arg(canBusStateText(state), description());
    if (state == CanBusState::BusOff) {
        emit errorOccurred(text + tr("，断开后重新连接可恢复"));
    } else if (state == CanBusState::Normal) {
        emit infoMessage(text);
    } else {
        emit warningMessage(text);
    }
    emit busStateChanged(state);
}

void CanLink::onSendFailed(const QString &message)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (message == m_lastSendError && now - m_lastSendErrorAt < kRepeatErrorMs) {
        return;
    }
    m_lastSendError = message;
    m_lastSendErrorAt = now;
    emit errorOccurred(message);
}

} // namespace fhmi
