#pragma once

#include "comm/comm_hub.h"

#include <functional>
#include <memory>

namespace fhmi {

struct PeriodicTask
{
    QString id;
    QString title;
    QString targets;
    QString content;
    int intervalMs = 0;
    quint64 submitted = 0;
    quint64 skipped = 0;
    quint64 token = 0;
    bool paused = false;
    QString inputError;
};

// One task owner for a running project. CAN schedules at the link's frame slot
// so that legacy periods and manual tasks share its transmit image and timer.
class PeriodicSender final : public QObject
{
    Q_OBJECT

  public:
    explicit PeriodicSender(CommHub *hub, QObject *parent = nullptr);

    bool start(const QString &id, const QString &title, const EncodedCommand &command, const QString &mode,
               const QList<SendTarget> &targets, int intervalMs, std::function<void()> onFirstSend, QString *error,
               CommandReader readInputs);
    bool stop(const QString &id);
    void stopTask(quint64 token);
    void stopAll();
    bool running(const QString &id) const { return m_jobs.contains(id); }
    bool starting(const QString &id) const;
    qsizetype count() const;
    QList<PeriodicTask> tasks() const;

  signals:
    void changed(const QString &id);
    void failed(const QString &message);

  private:
    struct Job
    {
        PeriodicTask display;
        EncodedCommand command;
        QList<SendTarget> targets;
        quint64 token;
        QTimer *timer = nullptr;
        std::function<void()> onFirstSend;
        CommandReader readInputs;
        QString mode;
    };

    static void updateInputPreview(Job &job, const EncodedCommand &command);
    void sendBytes(const std::shared_ptr<Job> &job, bool refreshInputs);
    void record(const std::shared_ptr<Job> &job, SendStatus result, const QString &error = {});
    void stopChangedTargets();

    CommHub *m_hub;
    QHash<QString, std::shared_ptr<Job>> m_jobs;
};

} // namespace fhmi
