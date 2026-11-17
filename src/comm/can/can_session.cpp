#include "comm/can/can_session.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

namespace fhmi {

class SessionWorker : public CanBackend
{
    Q_OBJECT
  public:
    explicit SessionWorker(CanSession::Factory factory) : m_factory(std::move(factory)) {}
    bool open(const CanSettings &settings, QString *error) override
    {
        m_backend = m_factory(settings.adapter, error);
        if (!m_backend)
            return false;
        m_backend->setTransmitState(transmitState());
        connect(m_backend.get(), &CanBackend::framesReceived, this, &CanBackend::framesReceived);
        connect(m_backend.get(), &CanBackend::framesSent, this, &CanBackend::framesSent);
        connect(m_backend.get(), &CanBackend::busStateChanged, this, &CanBackend::busStateChanged);
        connect(m_backend.get(), &CanBackend::frameSendFailed, this, &CanBackend::frameSendFailed);
        connect(m_backend.get(), &CanBackend::sendFailed, this, &CanBackend::sendFailed);
        connect(m_backend.get(), &CanBackend::failed, this, &CanBackend::failed);
        return m_backend->open(settings, error);
    }
    void close() override
    {
        if (m_backend) {
            m_backend->close();
            m_backend.reset();
        }
    }
    bool send(const CanFrame &frame, QString *error) override { return m_backend && m_backend->send(frame, error); }
    bool isOpen() const override { return m_backend && m_backend->isOpen(); }
    QString description() const override { return m_backend ? m_backend->description() : QString(); }
    void start(const CanSettings &settings)
    {
        if (transmitState()->stopping)
            return;
        QString error;
        const bool ok = open(settings, &error);
        emit ready(ok, error, description());
    }
  signals:
    void ready(bool success, const QString &error, const QString &description);

  private:
    CanSession::Factory m_factory;
    std::unique_ptr<CanBackend> m_backend;
};

CanSession::CanSession(Factory factory) : m_factory(std::move(factory)) {}
CanSession::~CanSession() { close(); }

bool CanSession::open(const CanSettings &settings, QString *)
{
    if (m_worker)
        return false;
    const auto state = std::make_shared<CanTransmitState>();
    setTransmitState(state);
    m_queued = std::make_shared<std::atomic<int>>(0);
    auto *worker = new SessionWorker(m_factory);
    worker->setTransmitState(state);
    auto *thread = new QThread;
    m_worker = worker;
    m_thread = thread;
    worker->moveToThread(thread);
    connect(worker, &CanBackend::framesReceived, this, &CanBackend::framesReceived);
    connect(worker, &CanBackend::framesSent, this, &CanBackend::framesSent);
    connect(worker, &CanBackend::busStateChanged, this, &CanBackend::busStateChanged);
    connect(worker, &CanBackend::frameSendFailed, this, &CanBackend::frameSendFailed);
    connect(worker, &CanBackend::sendFailed, this, &CanBackend::sendFailed);
    connect(worker, &CanBackend::failed, this, &CanBackend::failed);
    connect(worker, &SessionWorker::ready, this, [this](bool ok, const QString &error, const QString &description) {
        if (transmitState()->stopping)
            return;
        m_open = ok;
        m_description = description;
        emit opened(ok, error);
    });
    // Drain native ownership before process teardown can unload vendor libraries. Ordinary card
    // disconnect and destruction remain asynchronous; only final application exit joins workers.
    connect(qApp, &QCoreApplication::aboutToQuit, thread, [thread, state] {
        state->stopping = true;
        thread->quit();
        thread->wait();
    });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this] {
        m_worker = nullptr;
        m_thread = nullptr;
        emit closed();
    });
    connect(thread, &QThread::started, worker, [worker, settings] { worker->start(settings); });
    thread->start();
    return true;
}

void CanSession::close()
{
    m_open = false;
    if (!m_worker || transmitState()->stopping.exchange(true))
        return;
    auto *worker = static_cast<SessionWorker *>(m_worker.data());
    auto *thread = m_thread.data();
    QMetaObject::invokeMethod(
        worker,
        [worker, thread] {
            worker->close();
            thread->quit();
        },
        Qt::QueuedConnection);
}

bool CanSession::send(const CanFrame &frame, QString *error)
{
    const auto state = transmitState();
    if (!m_open || state->stopping) {
        if (error)
            *error = tr("CAN 通道未打开");
        return false;
    }
    const quint64 generation = state->generation;
    if (state->busOff) {
        if (error)
            *error = busOffSendError();
        return false;
    }
    if (*m_queued >= 4096) {
        if (error)
            *error = tr("CAN 发送队列已满");
        return false;
    }
    ++*m_queued;
    const auto queued = m_queued;
    auto *worker = static_cast<SessionWorker *>(m_worker.data());
    const bool queuedForDelivery = QMetaObject::invokeMethod(
        worker,
        [worker, queued, frame, state, generation] {
            --*queued;
            if (state->stopping)
                return;
            if (state->busOff || generation != state->generation) {
                emit worker->frameSendFailed(frame, busOffSendError());
                return;
            }
            QString error;
            if (!worker->send(frame, &error))
                emit worker->frameSendFailed(frame, error);
        },
        Qt::QueuedConnection);
    if (!queuedForDelivery) {
        --*queued;
        if (error)
            *error = tr("CAN 通道未打开");
    }
    return queuedForDelivery;
}

} // namespace fhmi

#include "can_session.moc"
