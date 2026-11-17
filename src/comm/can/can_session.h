#pragma once

#include "comm/can/can_backend.h"
#include <QtCore/QPointer>
#include <atomic>
#include <functional>

class QThread;

namespace fhmi {

// Owns a backend on its event-loop thread. Opening, polling shutdown and native close never run
// on the caller's thread. The worker finishes cleanup even when its card has been destroyed.
class CanSession : public CanBackend
{
    Q_OBJECT
  public:
    using Factory = std::function<std::unique_ptr<CanBackend>(const QString &, QString *)>;
    explicit CanSession(Factory factory = CanBackend::create);
    ~CanSession() override;
    bool open(const CanSettings &settings, QString *error) override;
    void close() override;
    bool send(const CanFrame &frame, QString *error) override;
    bool isOpen() const override { return m_open; }
    QString description() const override { return m_description; }

  signals:
    void opened(bool success, const QString &error);
    void closed();

  private:
    std::shared_ptr<std::atomic<int>> m_queued;
    Factory m_factory;
    QPointer<QObject> m_worker;
    QPointer<QThread> m_thread;
    bool m_open = false;
    QString m_description;
};

} // namespace fhmi
