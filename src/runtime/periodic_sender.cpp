#include "runtime/periodic_sender.h"

#include "comm/can/can_link.h"

#include <QtCore/QTimer>

#include <algorithm>
#include <utility>

namespace fhmi {
namespace {

std::optional<CanLink::Prepared> prepareCan(const CanLink &link, const EncodedCommand &command, QString *error)
{
    return command.kind == EncodedCommand::Kind::CanFrame
        ? link.prepareRawFrame(command.frame, error) : link.prepare(command.can, error);
}

QString taskContent(const EncodedCommand &command, const QString &mode)
{
    return command.inputValues.isEmpty() ? CommandEncoder::describe(command, mode)
                                         : CommandEncoder::inputValuesText(command);
}

} // namespace

PeriodicSender::PeriodicSender(CommHub *hub, QObject *parent) : QObject(parent), m_hub(hub)
{
    connect(hub, &CommHub::linksChanged, this, &PeriodicSender::stopChangedTargets);
    connect(hub, &CommHub::canLegacyCyclesChanged, this, [this] { emit changed({}); });
    connect(hub, &CommHub::canPeriodicResult, this, [this](quint64 token, SendStatus status, const QString &error) {
        const auto it = std::find_if(m_jobs.cbegin(), m_jobs.cend(), [token](const auto &job) { return job->token == token; });
        if (it != m_jobs.cend()) {
            const auto job = *it;
            record(job, status, error);
        }
    });
}

bool PeriodicSender::start(const QString &id, const QString &title, const EncodedCommand &command, const QString &mode,
                           const QList<SendTarget> &targets, int intervalMs, std::function<void()> onFirstSend,
                           QString *error, CommandReader readInputs)
{
    if (targets.isEmpty() || targets != m_hub->sendTargets(command)) {
        *error = tr("发送目标已变化，请重新下发");
        return false;
    }
    if (running(id))
        return false;

    QList<QPair<CanLink *, CanLink::Prepared>> canTargets;
    QStringList names;
    for (const auto &target : targets) {
        QString name = target.label();
        if (auto *link = qobject_cast<CanLink *>(target.link.data())) {
            const auto prepared = prepareCan(*link, command, error);
            if (!prepared)
                return false;
            const auto &frame = prepared->frame;
            name += QStringLiteral(" · ") + Can::idText({frame.id, frame.extended});
            canTargets.append({link, *prepared});
        }
        names.append(name);
    }
    const auto content = taskContent(command, mode);
    const auto token = nextDataStream();
    const auto job = std::make_shared<Job>(Job{{id, title, names.join(u'\n'), content, intervalMs, 0, 0, token, false, {}},
                                              command, targets, token, nullptr, std::move(onFirstSend),
                                              std::move(readInputs), mode});
    m_jobs.insert(id, job);
    for (const auto &target : std::as_const(canTargets)) {
        auto *link = target.first;
        CanLink::PeriodicFrameReader readFrame;
        if (job->readInputs) {
            readFrame = [job, link](QString *error) -> std::optional<CanLink::Prepared> {
                const auto result = job->readInputs();
                if (!result.command) {
                    *error = result.error;
                    return std::nullopt;
                }
                const auto frame = prepareCan(*link, *result.command, error);
                if (frame)
                    updateInputPreview(*job, *result.command);
                return frame;
            };
        }
        if (!link->startPeriodic(target.second, job->token, title, intervalMs, error, std::move(readFrame))) {
            stop(id);
            return false;
        }
    }
    if (canTargets.isEmpty()) {
        job->timer = new QTimer(this);
        job->timer->setTimerType(Qt::PreciseTimer);
        connect(job->timer, &QTimer::timeout, this, [this, job] { sendBytes(job, true); });
        job->timer->start(intervalMs);
    }
    emit changed(id);
    if (job->timer)
        sendBytes(job, false);
    return true;
}

bool PeriodicSender::stop(const QString &id)
{
    const auto job = m_jobs.take(id);
    if (!job)
        return false;
    if (job->timer) {
        job->timer->stop();
        job->timer->deleteLater();
    }
    for (const auto &target : job->targets) {
        if (auto *link = qobject_cast<CanLink *>(target.link.data()))
            link->stopPeriodic(job->token);
    }
    emit changed(id);
    return true;
}

void PeriodicSender::stopAll()
{
    for (const auto &id : m_jobs.keys())
        stop(id);
    for (auto *link : m_hub->connectedCanLinks())
        link->stopLegacyCycles();
}

void PeriodicSender::stopTask(quint64 token)
{
    for (const auto &job : m_jobs.values()) {
        if (job->token == token) {
            stop(job->display.id);
            return;
        }
    }
    for (auto *link : m_hub->connectedCanLinks()) {
        if (link->stopLegacyCycles(token))
            return;
    }
}

qsizetype PeriodicSender::count() const
{
    qsizetype count = m_jobs.size();
    for (auto *link : m_hub->connectedCanLinks())
        count += link->legacyCycles().size();
    return count;
}

bool PeriodicSender::starting(const QString &id) const
{
    const auto job = m_jobs.value(id);
    return job && job->display.submitted == 0;
}

QList<PeriodicTask> PeriodicSender::tasks() const
{
    QList<PeriodicTask> items;
    for (const auto &job : m_jobs)
        items.append(job->display);
    for (auto *link : m_hub->connectedCanLinks()) {
        for (const auto &cycle : link->legacyCycles()) {
            const auto id = Can::idText({cycle.frame.id, cycle.frame.extended});
            items.append({id, tr("CAN 报文 %1 周期").arg(id), link->connectionId() + QStringLiteral(" · ") + id,
                          Can::frameLine(cycle.frame), cycle.intervalMs, cycle.submitted, cycle.skipped, cycle.task,
                          cycle.paused, {}});
        }
    }
    std::sort(items.begin(), items.end(), [](const PeriodicTask &a, const PeriodicTask &b) { return a.token < b.token; });
    return items;
}

void PeriodicSender::updateInputPreview(Job &job, const EncodedCommand &command)
{
    job.display.content = taskContent(command, job.mode);
    job.display.inputError.clear();
}

void PeriodicSender::sendBytes(const std::shared_ptr<Job> &job, bool refreshInputs)
{
    const auto &id = job->display.id;
    if (m_jobs.value(id) != job)
        return;
    auto bytes = job->command.bytes;
    if (refreshInputs && job->readInputs) {
        const auto result = job->readInputs();
        if (!result.command) {
            record(job, SendStatus::Busy, result.error);
            return;
        }
        bytes = result.command->bytes;
        updateInputPreview(*job, *result.command);
    }
    for (const auto &target : job->targets) {
        if (m_jobs.value(id) != job)
            return;
        const auto status = target.isCurrent()
            ? target.link->sendPeriodic(bytes, target.endpoint) : SendStatus::Failed;
        record(job, status);
    }
}

void PeriodicSender::record(const std::shared_ptr<Job> &job, SendStatus result, const QString &error)
{
    const auto &id = job->display.id;
    if (m_jobs.value(id) != job)
        return;
    switch (result) {
    case SendStatus::Submitted:
        if (++job->display.submitted == 1) {
            emit changed(id);
            if (auto callback = std::move(job->onFirstSend))
                callback();
        }
        break;
    case SendStatus::Busy:
        ++job->display.skipped;
        if (!error.isEmpty())
            job->display.inputError = error;
        break;
    case SendStatus::Failed:
        stop(id);
        emit failed(error.isEmpty() ? tr("%1 发送失败，周期任务已停止").arg(job->display.title) : error);
        break;
    }
}

void PeriodicSender::stopChangedTargets()
{
    for (const auto &job : m_jobs.values()) {
        if (job->targets != m_hub->sendTargets(job->command) && stop(job->display.id))
            emit failed(tr("发送目标已变化，周期发送已停止"));
    }
    emit changed({});
}

} // namespace fhmi
