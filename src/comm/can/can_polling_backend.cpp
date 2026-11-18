#include "comm/can/can_polling_backend.h"
#include "comm/can/can_diagnostics.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDeadlineTimer>
#include <QtCore/QThread>

namespace fhmi {

namespace {

// Frames queued beyond this are refused: the adapter is not keeping up (a bus without a receiver, bus off).
constexpr qsizetype kMaxQueue = 4096;
constexpr int kStateIntervalMs = 500;
// Waiting between reads when nothing happens; a frame queued by send() wakes the worker at once.
constexpr int kIdleWaitMs = 2;

} // namespace

CanPollingBackend::~CanPollingBackend()
{
    // A subclass stops the worker in its own destructor, while its functions still exist; this is a last resort.
    stopWorker();
}

bool CanPollingBackend::send(const CanFrame &frame, QString *error)
{
    {
        QMutexLocker lock(&m_mutex);
        if (!m_running || transmitState()->stopping) {
            if (error) {
                *error = tr("CAN 通道未打开");
            }
            return false;
        }
        if (m_state == CanBusState::BusOff) {
            if (error)
                *error = busOffSendError();
            return false;
        }
        if (m_queue.size() >= kMaxQueue) {
            if (error) {
                *error = tr("发送队列已满，适配器没有把报文发出去（总线上没有其他节点或总线关闭？）");
            }
            return false;
        }
        m_queue.append(frame);
        m_wakeup.wakeOne();
    }
    wake();
    return true;
}

void CanPollingBackend::waitForActivity(int ms)
{
    QMutexLocker lock(&m_mutex);
    if (m_queue.isEmpty() && m_running && !transmitState()->stopping) {
        m_wakeup.wait(&m_mutex, QDeadlineTimer(ms));
    }
}

void CanPollingBackend::wake() {}

void CanPollingBackend::startWorker()
{
    stopWorker();
    m_state = CanBusState::Normal;
    transmitState()->observe(m_state);
    m_running = true;
    m_thread = QThread::create([this]() { run(); });
    m_thread->start(QThread::HighPriority);
}

void CanPollingBackend::stopWorker()
{
    if (!m_thread) {
        return;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_running = false;
        m_wakeup.wakeAll();
    }
    wake();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    QMutexLocker lock(&m_mutex);
    m_queue.clear();
    m_received.clear();
    m_sent.clear();
    m_rejected.clear();
    m_sendErrors.clear();
    m_failure.clear();
    m_stateChanged = false;
}

void CanPollingBackend::observeWorkerBusState(CanBusState state)
{
    QMutexLocker lock(&m_mutex);
    if (m_state == state)
        return;
    m_state = state;
    m_stateChanged = true;
    transmitState()->observe(state);
    if (state == CanBusState::BusOff) {
        const QString error = busOffSendError();
        for (const CanFrame &frame : std::as_const(m_queue)) {
            if (frame.submissionId || m_rejected.size() < 8)
                m_rejected.append({frame, error});
        }
        m_queue.clear();
    }
}

void CanPollingBackend::run()
{
    QList<CanFrame> received;
    QList<CanFrame> sent;
    qint64 nextState = 0;
    qint64 nextRecovery = 0;
    const auto running = [this] { return m_running && !transmitState()->stopping; };
    const bool queryState = CanDiagnostics::statusQueriesEnabled();
    if constexpr (CanDiagnostics::BuildEnabled) {
        CanDiagnostics::record(
            "worker_start", {{"backend", QString::number(quintptr(this), 16)}, {"status_queries_enabled", queryState}});
    }
    while (running()) {
        QList<CanFrame> outgoing;
        {
            QMutexLocker lock(&m_mutex);
            outgoing.swap(m_queue);
        }
        for (CanFrame &frame : outgoing) {
            if (!running())
                break;
            QString error;
            const bool busOff = transmitState()->busOff;
            if (busOff)
                error = busOffSendError();
            if (!busOff && writeFrame(frame, &error)) {
                frame.timestampMs = QDateTime::currentMSecsSinceEpoch();
                sent.append(frame);
            } else {
                QMutexLocker lock(&m_mutex);
                if (frame.submissionId || m_rejected.size() < 8)
                    m_rejected.append({frame, error});
            }
        }
        if (!running())
            break;
        QString failure;
        if (!readFrames(received, &failure)) {
            if constexpr (CanDiagnostics::BuildEnabled) {
                CanDiagnostics::record("worker_read_failed",
                                       {{"backend", QString::number(quintptr(this), 16)}, {"error", failure}});
            }
            QMutexLocker lock(&m_mutex);
            m_failure = failure;
            m_running = false;
        }
        const qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
        const qint64 receivedAt = Can::monotonicMs();
        for (CanFrame &frame : received) {
            frame.timestampMs = timestamp;
            frame.receivedAtMs = receivedAt;
        }
        if constexpr (CanDiagnostics::BuildEnabled) {
            if (!received.isEmpty()) {
                CanDiagnostics::record("worker_frames_read", {{"backend", QString::number(quintptr(this), 16)},
                                                              {"count", received.size()},
                                                              {"software_timestamp_ms", timestamp}});
            }
        }
        const bool traffic = !received.isEmpty() || !outgoing.isEmpty();
        // Post frames already acquired before a potentially slow vendor status/recovery call.
        post(received, sent);
        received.clear();
        sent.clear();
        if (queryState && receivedAt >= nextState && running()) {
            nextState = receivedAt + kStateIntervalMs;
            observeWorkerBusState(readBusState());
            post({}, {});
        }
        const qint64 now = Can::monotonicMs();
        if (m_autoRecover && transmitState()->busOff && now >= nextRecovery && running()) {
            nextRecovery = now + 1000;
            QString error;
            if (!recoverBus(&error) && !error.isEmpty()) {
                QMutexLocker lock(&m_mutex);
                if (m_sendErrors.size() < 8)
                    m_sendErrors.append(error);
            }
            post({}, {});
        }
        if (!traffic && running()) {
            waitForActivity(kIdleWaitMs);
        }
    }
    QList<CanFrame> none;
    post(none, none); // the failure, if any
    if constexpr (CanDiagnostics::BuildEnabled) {
        CanDiagnostics::record("worker_stop", {{"backend", QString::number(quintptr(this), 16)}});
    }
}

void CanPollingBackend::post(const QList<CanFrame> &received, const QList<CanFrame> &sent)
{
    QMutexLocker lock(&m_mutex);
    m_received += received;
    m_sent += sent;
    const bool news = !m_received.isEmpty() || !m_sent.isEmpty() || !m_rejected.isEmpty() || !m_sendErrors.isEmpty() ||
                      !m_failure.isEmpty() || m_stateChanged;
    if constexpr (CanDiagnostics::BuildEnabled) {
        if (!received.isEmpty() || !sent.isEmpty()) {
            CanDiagnostics::record("worker_post", {{"backend", QString::number(quintptr(this), 16)},
                                                   {"received", received.size()},
                                                   {"sent", sent.size()},
                                                   {"pending_received", m_received.size()}});
        }
    }
    if (news && !m_deliveryPending) {
        // One delivery per event-loop pass: everything that arrives meanwhile goes with it.
        m_deliveryPending = true;
        QMetaObject::invokeMethod(this, &CanPollingBackend::deliver, Qt::QueuedConnection);
    }
}

void CanPollingBackend::deliver()
{
    QList<CanFrame> received;
    QList<CanFrame> sent;
    QList<QPair<CanFrame, QString>> rejected;
    QStringList sendErrors;
    QString failure;
    bool stateChanged = false;
    CanBusState state = CanBusState::Normal;
    {
        QMutexLocker lock(&m_mutex);
        m_deliveryPending = false;
        received.swap(m_received);
        sent.swap(m_sent);
        rejected.swap(m_rejected);
        sendErrors.swap(m_sendErrors);
        failure.swap(m_failure);
        stateChanged = m_stateChanged;
        m_stateChanged = false;
        state = m_state;
    }
    if constexpr (CanDiagnostics::BuildEnabled) {
        CanDiagnostics::record(
            "backend_delivery", {{"backend", QString::number(quintptr(this), 16)},
                             {"received", received.size()},
                             {"sent", sent.size()},
                             {"oldest_software_timestamp_ms", received.isEmpty() ? 0 : received.front().timestampMs}});
    }
    // Sent first: a reply can only follow its request.
    if (!sent.isEmpty()) {
        emit framesSent(sent);
    }
    if (!received.isEmpty()) {
        emit framesReceived(received);
    }
    if (stateChanged) {
        emit busStateChanged(state);
    }
    for (const auto &[frame, error] : std::as_const(rejected))
        emit frameSendFailed(frame, error);
    for (const QString &error : std::as_const(sendErrors)) {
        emit sendFailed(error);
    }
    if (!failure.isEmpty()) {
        emit failed(failure);
    }
}

} // namespace fhmi
