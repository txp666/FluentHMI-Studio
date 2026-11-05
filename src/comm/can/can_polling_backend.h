#pragma once

#include "comm/can/can_backend.h"

#include <QtCore/QMutex>
#include <QtCore/QWaitCondition>

#include <atomic>

class QThread;

namespace fhmi {

// A backend whose vendor library is read by polling (PCAN-Basic, zlgcan): a worker thread writes the queued
// frames, reads what arrived and checks the bus state every 500 ms; the GUI thread only queues and collects.
// Subclasses open the channel in open() and then call startWorker(); their close() and destructor call
// stopWorker() before releasing the channel, since the worker calls their virtual functions.
class CanPollingBackend : public CanBackend
{
    Q_OBJECT

  public:
    using CanBackend::CanBackend;
    ~CanPollingBackend() override;

    bool send(const CanFrame &frame, QString *error) override;

  protected:
    // Worker thread. Appends what arrived (any number, at most a few hundred); false when the adapter is gone,
    // with *error saying so.
    virtual bool readFrames(QList<CanFrame> &out, QString *error) = 0;
    // Worker thread. Writes one frame; false with *error when the adapter refused it.
    virtual bool writeFrame(const CanFrame &frame, QString *error) = 0;
    // Worker thread, every 500 ms.
    virtual CanBusState readBusState() = 0;
    // Worker thread: waits up to `ms` for frames to arrive or send(); the default only wakes on send().
    virtual void waitForActivity(int ms);
    // Any thread: ends waitForActivity() early.
    virtual void wake();

    void startWorker();
    void stopWorker();

  private:
    void run();
    void post(QList<CanFrame> &received, QList<CanFrame> &sent);
    void deliver();

    QThread *m_thread = nullptr;
    std::atomic<bool> m_running{false};
    QMutex m_mutex; // guards everything below
    QWaitCondition m_wakeup;
    QList<CanFrame> m_queue;    // to send
    QList<CanFrame> m_received; // for the next delivery
    QList<CanFrame> m_sent;
    QStringList m_sendErrors;
    QString m_failure;
    bool m_stateChanged = false;
    CanBusState m_state = CanBusState::Normal;
    bool m_deliveryPending = false;
};

} // namespace fhmi
