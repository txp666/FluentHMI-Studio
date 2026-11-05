#include "comm/can/can_polling_backend.h"

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
        if (!m_running) {
            if (error) {
                *error = tr("CAN 通道未打开");
            }
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
    if (m_queue.isEmpty() && m_running) {
        m_wakeup.wait(&m_mutex, QDeadlineTimer(ms));
    }
}

void CanPollingBackend::wake() {}

void CanPollingBackend::startWorker()
{
    stopWorker();
    m_state = CanBusState::Normal;
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
    m_sendErrors.clear();
    m_failure.clear();
    m_stateChanged = false;
}

void CanPollingBackend::run()
{
    QList<CanFrame> received;
    QList<CanFrame> sent;
    qint64 nextState = 0;
    CanBusState state = CanBusState::Normal;
    while (m_running) {
        QList<CanFrame> outgoing;
        {
            QMutexLocker lock(&m_mutex);
            outgoing.swap(m_queue);
        }
        for (CanFrame &frame : outgoing) {
            QString error;
            if (writeFrame(frame, &error)) {
                frame.timestampMs = QDateTime::currentMSecsSinceEpoch();
                sent.append(frame);
            } else {
                QMutexLocker lock(&m_mutex);
                if (m_sendErrors.size() < 8) {
                    m_sendErrors.append(error);
                }
            }
        }
        QString failure;
        const qsizetype before = received.size();
        if (!readFrames(received, &failure)) {
            QMutexLocker lock(&m_mutex);
            m_failure = failure;
            m_running = false;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (qsizetype i = before; i < received.size(); ++i) {
            received[i].timestampMs = now;
        }
        if (now >= nextState && m_running) {
            nextState = now + kStateIntervalMs;
            const CanBusState current = readBusState();
            if (current != state) {
                state = current;
                QMutexLocker lock(&m_mutex);
                m_state = current;
                m_stateChanged = true;
            }
        }
        post(received, sent);
        if (received.isEmpty() && outgoing.isEmpty() && m_running) {
            waitForActivity(kIdleWaitMs);
        }
        received.clear();
        sent.clear();
    }
    QList<CanFrame> none;
    post(none, none); // the failure, if any
}

void CanPollingBackend::post(QList<CanFrame> &received, QList<CanFrame> &sent)
{
    QMutexLocker lock(&m_mutex);
    m_received += received;
    m_sent += sent;
    const bool news = !m_received.isEmpty() || !m_sent.isEmpty() || !m_sendErrors.isEmpty() ||
                      !m_failure.isEmpty() || m_stateChanged;
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
    QStringList sendErrors;
    QString failure;
    bool stateChanged = false;
    CanBusState state = CanBusState::Normal;
    {
        QMutexLocker lock(&m_mutex);
        m_deliveryPending = false;
        received.swap(m_received);
        sent.swap(m_sent);
        sendErrors.swap(m_sendErrors);
        failure.swap(m_failure);
        stateChanged = m_stateChanged;
        m_stateChanged = false;
        state = m_state;
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
    for (const QString &error : std::as_const(sendErrors)) {
        emit sendFailed(error);
    }
    if (!failure.isEmpty()) {
        emit failed(failure);
    }
}

} // namespace fhmi
