#include "runtime/modbus_poller.h"

#include "comm/modbus_slave.h"
#include "runtime/runtime_context.h"

#include <QtCore/QTimer>

#include <algorithm>

namespace fhmi {

namespace {

// A block counts as due this much early, so a tick just before its time does not leave it waiting.
constexpr qint64 kDueSlackMs = 10;

} // namespace

ModbusPoller::ModbusPoller(RuntimeContext *context) : QObject(context), m_context(context), m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::PreciseTimer); // a block's period is kept to the tick
    connect(m_timer, &QTimer::timeout, this, &ModbusPoller::poll);
    // When the block due next falls between two ticks.
    m_wake = new QTimer(this);
    m_wake->setSingleShot(true);
    m_wake->setTimerType(Qt::PreciseTimer);
    connect(m_wake, &QTimer::timeout, this, &ModbusPoller::poll);
    // A link that connects, or a listening server that gets a device, is read from at once.
    connect(context->hub(), &CommHub::linksChanged, this, &ModbusPoller::poll);
}

ModbusPoller::Settings ModbusPoller::settings(Link *link) const
{
    for (const LinkCard &entry : m_cards) {
        if (entry.link && (link ? entry.link == link : entry.link->isConnected() && entry.link->parsesModbus())) return entry.settings;
    }
    return {};
}

void ModbusPoller::setLinkCard(const QObject *card, Link *link, const Settings &settings)
{
    auto it = std::find_if(m_cards.begin(), m_cards.end(), [card](const LinkCard &entry) { return entry.card == card; });
    if (it == m_cards.end()) m_cards.append({card, link, settings});
    else *it = {card, link, settings};
    poll();
}

void ModbusPoller::removeLinkCard(const QObject *card)
{
    m_cards.removeIf([card](const LinkCard &entry) { return entry.card == card; });
    poll();
}

void ModbusPoller::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    if (active) {
        m_timer->start(std::max(1, settings().defaultPeriod));
        poll();
    } else halt();
}

void ModbusPoller::halt()
{
    m_timer->stop();
    m_wake->stop();
    m_channels.clear();
    m_blocks.clear();
}

void ModbusPoller::reset()
{
    halt();
    m_active = false;
    m_waiting = false;
    m_cards.clear();
    m_status.clear();
    m_toolTip.clear();
}

void ModbusPoller::replan(Channel &channel, const QList<ModbusNeed> &needs)
{
    QList<ModbusNeed> active;
    const auto source = channel.link->eventSource(channel.endpoint);
    for (ModbusNeed need : needs) {
        auto route = need.route;
        if (!channel.link->addressedModbusReplies()) route.endpoint.clear();
        if (!need.visible || !route.accepts(source)) continue;
        if (need.slave <= 0) need.slave = channel.settings.fallbackSlave;
        if (need.slave < 1 || need.slave > 247 || need.address < 0 || need.address > 65535) continue;
        const auto slaves = m_context->hub()->modbusSlaves(need.route);
        if (std::any_of(slaves.cbegin(), slaves.cend(), [&need](const ModbusSlave *slave) { return slave->settings().slave == need.slave; })) continue;
        need.period = std::max(0, need.period);
        active.append(need);
    }
    const auto blocks = planBlocks(active, std::max(1, channel.settings.defaultPeriod));
    if (blocks == channel.blocks) {
        if (blocks.isEmpty()) describe(channel, tr("没有 Modbus 绑定"));
        return;
    }
    QList<qint64> due(blocks.size(), 0);
    for (int i = 0; i < blocks.size(); ++i) {
        const auto old = channel.blocks.indexOf(blocks.at(i));
        if (old >= 0) due[i] = channel.due.at(old);
    }
    channel.blocks = blocks;
    channel.due = due;
    channel.results = QList<std::optional<ModbusResult>>(blocks.size());
    ++channel.plan;
    describe(channel, blocks.isEmpty() ? tr("没有 Modbus 绑定") : tr("%1 块 · 轮询中").arg(blocks.size()));
}

void ModbusPoller::poll()
{
    if (!m_active || m_polling) return;
    m_polling = true;
    m_wake->stop();
    const auto needs = m_context->modbusNeeds();
    QSet<ModbusMaster *> current;
    int interval = std::max(1, settings().defaultPeriod);
    qint64 wake = interval;
    const qint64 now = m_context->now();
    for (Link *link : m_context->hub()->connectedLinks()) {
        if (!link->parsesModbus()) continue;
        QStringList endpoints;
        if (link->addressedModbusReplies()) {
            if (link->canSend()) endpoints = link->sendEndpoints();
            for (const auto &need : needs) {
                if (need.visible && !need.route.endpoint.isEmpty() &&
                    need.route.acceptsConnection(link->connectionId()) && link->canSendTo(need.route.endpoint))
                    endpoints.append(need.route.endpoint);
            }
            endpoints.removeDuplicates();
        } else if (link->canSendTo({})) {
            // MQTT replies can come from a different topic than the polling destination.
            endpoints.append(QString());
        }
        for (const QString &endpoint : endpoints) {
            auto *master = link->modbusMaster(endpoint);
            current.insert(master);
            auto channel = m_channels.value(master);
            if (!channel || channel->master != master) {
                channel = std::make_shared<Channel>();
                channel->link = link;
                channel->master = master;
                channel->endpoint = endpoint;
                m_channels.insert(master, channel);
                connect(master, &ModbusMaster::queueIdle, this, &ModbusPoller::poll, Qt::UniqueConnection);
            }
            channel->settings = settings(link);
            replan(*channel, needs);
            interval = std::min(interval, std::max(1, channel->settings.defaultPeriod));
            for (const auto &block : channel->blocks) interval = std::min(interval, block.period);
            if (channel->blocks.isEmpty()) continue;
            if (master->busy() || master->pending())
                continue;
            const auto due = *std::min_element(channel->due.cbegin(), channel->due.cend());
            int next = -1;
            for (int i = 0; i < channel->blocks.size(); ++i) {
                if (channel->due.at(i) <= now + kDueSlackMs && (next < 0 || channel->due.at(i) < channel->due.at(next))) next = i;
            }
            if (next < 0) {
                wake = std::min(wake, due - now);
                continue;
            }
            const auto block = channel->blocks.at(next);
            channel->due[next] = now + block.period;
            const ModbusRequest request{block.slave, block.fc, block.start, block.count, {}};
            const int plan = channel->plan;
            const std::weak_ptr<Channel> weak = channel;
            const auto frame = master->enqueue(request, channel->settings.timeoutMs, ModbusMaster::Priority::Read,
                                               [this, weak, next, plan](const ModbusResult &result) {
                if (auto alive = weak.lock()) blockDone(alive, next, plan, result);
            });
            if (frame.isEmpty()) {
                ModbusResult failed;
                failed.problem = tr("发送失败");
                blockDone(channel, next, plan, failed);
            }
        }
    }
    m_channels.removeIf([&current](const auto &item) { return !current.contains(item.key()); });
    m_waiting = current.isEmpty();
    updateStatus();
    m_timer->setInterval(std::max(1, interval));
    m_polling = false;
    if (!m_waiting && !m_wake->isActive()) m_wake->start(int(std::max<qint64>(1, wake)));
}

void ModbusPoller::blockDone(const std::shared_ptr<Channel> &channel, int block, int plan, const ModbusResult &result)
{
    if (!m_active || result.cancelled) return;
    if (plan == channel->plan && block < channel->results.size()) {
        if (!result.ok && channel->link) {
            m_context->modbusReadFailed(*channel->link, channel->endpoint, channel->blocks.at(block),
                                        channel->settings.fallbackSlave);
        }
        auto &slot = channel->results[block];
        if (!slot || slot->ok) slot = result;
        if (std::all_of(channel->results.cbegin(), channel->results.cend(), [](const auto &r) { return r.has_value(); })) {
            QString status = tr("%1 块 · 正常").arg(channel->blocks.size());
            for (int k = 0; k < channel->blocks.size(); ++k) {
                const auto &r = *channel->results.at(k);
                if (r.ok) continue;
                const auto b = channel->blocks.at(k);
                const QString exception = modbusException(r.message);
                status = exception.isEmpty() ? tr("第 %1 块无响应（从站 %2 功能码 %3 地址 %4）").arg(k + 1).arg(b.slave).arg(b.fc).arg(b.start)
                                             : tr("第 %1 块%2").arg(k + 1).arg(exception);
                break;
            }
            channel->results = QList<std::optional<ModbusResult>>(channel->blocks.size());
            describe(*channel, status);
        }
    }
    if (m_polling) m_wake->start(0);
    else poll();
}

void ModbusPoller::describe(Channel &channel, const QString &status)
{
    channel.status = status;
    QStringList lines;
    for (const auto &block : channel.blocks) {
        lines << tr("从站 %1 · 功能码 %2 · 地址 %3–%4 · 周期 %5 ms").arg(block.slave).arg(block.fc).arg(block.start).arg(block.start + block.count - 1).arg(block.period);
    }
    channel.tip = lines.isEmpty() ? status : lines.join(QLatin1Char('\n'));
}

QString ModbusPoller::status(const Link *link) const
{
    if (!link) return m_status;
    QStringList lines;
    const auto count = std::count_if(m_channels.cbegin(), m_channels.cend(), [link](const auto &channel) { return channel->link == link; });
    for (const auto &channel : m_channels) if (channel->link == link) {
        lines << ((channel->endpoint.isEmpty() || count == 1) ? channel->status : channel->endpoint + QStringLiteral(": ") + channel->status);
    }
    return lines.join(QLatin1Char('\n'));
}

QString ModbusPoller::toolTip(const Link *link) const
{
    if (!link) return m_toolTip;
    QStringList lines;
    for (const auto &channel : m_channels) if (channel->link == link) lines << channel->tip;
    return lines.join(QLatin1Char('\n'));
}

void ModbusPoller::updateStatus()
{
    QStringList statuses, tips;
    m_blocks.clear();
    // Preserve card order even when the user connects B before A.
    const auto connected = m_context->hub()->connectedLinks();
    QList<Link *> ordered;
    for (const auto &card : m_cards) if (card.link && connected.contains(card.link)) ordered.append(card.link);
    for (auto *link : connected) if (!ordered.contains(link)) ordered.append(link);
    for (Link *link : ordered) {
        for (const auto &channel : m_channels) if (channel->link == link) {
            const QString prefix = m_channels.size() > 1 ? link->connectionId() + (channel->endpoint.isEmpty() ? QString() : QLatin1Char('/') + channel->endpoint) + QStringLiteral(": ") : QString();
            statuses << prefix + channel->status;
            tips << prefix + channel->tip;
            m_blocks += channel->blocks;
        }
    }
    const QString status = m_waiting ? tr("等待连接") : statuses.join(QLatin1Char('\n'));
    const QString tip = m_waiting ? status : tips.join(QLatin1Char('\n'));
    if (status == m_status && tip == m_toolTip) return;
    m_status = status;
    m_toolTip = tip;
    emit statusChanged(status, tip);
}

} // namespace fhmi
