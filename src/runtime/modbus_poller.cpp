#include "runtime/modbus_poller.h"

#include "runtime/runtime_context.h"

#include <QtCore/QTimer>

#include <algorithm>

namespace fhmi {

namespace {

// A block counts as due this much early, so a tick that arrives a moment before a block's time does
// not leave it waiting a whole further tick.
constexpr qint64 kDueSlackMs = 10;

} // namespace

// ---------------------------------------------------------------------------
// ModbusExchange
// ---------------------------------------------------------------------------

bool ModbusExchange::start(const Sender &send, ModbusSend how, Done done)
{
    // A request still on its way goes out all the same; only its result is no longer reported.
    auto state = std::make_shared<State>();
    state->done = std::move(done);
    m_state = state;
    QList<QPointer<ModbusMaster>> masters;
    const std::weak_ptr<State> weak = state;
    how.owner = state.get();
    how.queuedOn = &masters;
    how.done = [this, weak](const ModbusResult &result) {
        if (const std::shared_ptr<State> alive = weak.lock()) {
            report(alive, result);
        }
    };
    if (!send(how)) {
        if (m_state == state) {
            m_state.reset();
        }
        return false;
    }
    if (m_state != state) {
        return true; // done, or replaced, while it went out
    }
    if (masters.isEmpty()) {
        m_state.reset(); // only links that do not parse Modbus took it: no reply to wait for
        return true;
    }
    for (const QPointer<ModbusMaster> &master : std::as_const(masters)) {
        if (!state->answered.contains(master.data())) {
            state->waiting.append(master);
        }
    }
    state->armed = true;
    settle(state);
    return true;
}

void ModbusExchange::report(const std::shared_ptr<State> &state, const ModbusResult &result)
{
    if (!state->result || (result.ok && !state->result->ok)) {
        state->result = result;
    }
    if (!state->armed) {
        state->answered.append(result.master.data());
        return;
    }
    state->waiting.removeAll(result.master);
    settle(state);
}

void ModbusExchange::settle()
{
    if (const std::shared_ptr<State> state = m_state) {
        settle(state);
    }
}

void ModbusExchange::settle(const std::shared_ptr<State> &state)
{
    if (!state->armed || state->finished) {
        return;
    }
    state->waiting.removeIf([](const QPointer<ModbusMaster> &master) { return master.isNull(); });
    if (!state->waiting.isEmpty()) {
        return;
    }
    state->finished = true;
    if (m_state == state) {
        m_state.reset();
    }
    ModbusResult result;
    if (state->result) {
        result = *state->result;
    } else { // every link it went to is gone
        result.timedOut = true;
        result.cancelled = true;
    }
    if (state->done) {
        state->done(result);
    }
}

void ModbusExchange::cancel()
{
    const std::shared_ptr<State> state = std::move(m_state);
    m_state.reset();
    if (!state) {
        return;
    }
    for (const QPointer<ModbusMaster> &master : std::as_const(state->waiting)) {
        if (master) {
            master->cancelQueued(state.get());
        }
    }
}

// ---------------------------------------------------------------------------
// ModbusPoller
// ---------------------------------------------------------------------------

ModbusPoller::ModbusPoller(RuntimeContext *context) : QObject(context), m_context(context), m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::PreciseTimer); // a block's period is kept to the tick
    connect(m_timer, &QTimer::timeout, this, &ModbusPoller::poll);
    // A link that connects, or a listening server that gets a device, is read from at once.
    connect(context->hub(), &CommHub::linksChanged, this, &ModbusPoller::poll);
}

ModbusPoller::Settings ModbusPoller::settings() const
{
    return m_panels.isEmpty() ? Settings{} : m_panels.first().second;
}

void ModbusPoller::addPanel(const QObject *panel, const Settings &settings)
{
    for (const auto &entry : std::as_const(m_panels)) {
        if (entry.first == panel) {
            setSettings(panel, settings);
            return;
        }
    }
    const Settings before = this->settings();
    m_panels.append({panel, settings});
    settingsChanged(before);
}

void ModbusPoller::setSettings(const QObject *panel, const Settings &settings)
{
    const Settings before = this->settings();
    for (auto &entry : m_panels) {
        if (entry.first == panel) {
            entry.second = settings;
        }
    }
    settingsChanged(before);
}

void ModbusPoller::removePanel(const QObject *panel)
{
    const Settings before = settings();
    m_panels.removeIf([panel](const std::pair<const QObject *, Settings> &entry) { return entry.first == panel; });
    settingsChanged(before);
}

void ModbusPoller::settingsChanged(const Settings &before)
{
    if (settings() != before) {
        poll(); // the plan, the timer and the timeout follow at once
    }
}

void ModbusPoller::pause()
{
    if (m_paused) {
        return;
    }
    m_paused = true;
    halt();
    setStatus(tr("已暂停"));
    emit pausedChanged(true);
}

void ModbusPoller::resume()
{
    if (!m_paused) {
        return;
    }
    m_paused = false;
    emit pausedChanged(false);
    if (m_active) {
        begin();
    }
}

void ModbusPoller::setActive(bool active)
{
    if (active == m_active) {
        return;
    }
    m_active = active;
    if (!active) {
        halt();
    } else if (!m_paused) {
        begin();
    }
}

void ModbusPoller::reset()
{
    halt();
    m_active = false;
    m_panels.clear();
    m_status.clear();
    m_toolTip.clear();
    if (m_paused) {
        m_paused = false;
        emit pausedChanged(false);
    }
}

void ModbusPoller::begin()
{
    m_read.cancel();
    clearPlan();
    m_timer->start(std::max(1, settings().defaultPeriod));
    poll(); // the first block goes out right away
}

void ModbusPoller::halt()
{
    m_timer->stop();
    m_read.cancel(); // its reads still waiting in a queue never go out; one on the line gets its reply
    clearPlan();
}

void ModbusPoller::clearPlan()
{
    m_blocks.clear();
    m_due.clear();
    m_results.clear();
    ++m_plan;
}

QList<Link *> ModbusPoller::modbusLinks() const
{
    QList<Link *> links;
    for (Link *link : m_context->hub()->connectedLinks()) {
        if (link->parsesModbus() && link->canSend()) {
            links.append(link);
        }
    }
    return links;
}

void ModbusPoller::poll()
{
    if (!m_active || m_paused) {
        return;
    }
    m_read.settle(); // a link that went away never answers
    const QList<Link *> links = modbusLinks();
    if (links.isEmpty()) {
        clearPlan(); // a link that comes back starts with a fresh plan
        setStatus(tr("等待连接"));
        return;
    }
    replan();
    updateInterval();
    if (m_blocks.isEmpty() || m_read.isActive()) {
        return;
    }
    for (Link *link : links) {
        if (link->modbusMaster()->busy()) {
            return; // a read, a write or a card's 发送 is on the line
        }
    }
    const qint64 now = m_context->now();
    int next = -1;
    for (int i = 0; i < int(m_blocks.size()); ++i) {
        if (m_due.at(i) <= now + kDueSlackMs && (next < 0 || m_due.at(i) < m_due.at(next))) {
            next = i;
        }
    }
    if (next < 0) {
        return;
    }
    const ModbusBlock block = m_blocks.at(next);
    m_due[next] = now + block.period;
    const auto request = CommandEncoder::modbusRequest(block.slave, block.fc, block.start, block.count);
    if (!request) {
        return; // not reached: the planner keeps blocks within what a request can read
    }
    ModbusSend how;
    how.timeoutMs = settings().timeoutMs;
    how.priority = ModbusMaster::Priority::Read;
    how.modbusLinksOnly = true;
    CommHub *hub = m_context->hub();
    const int plan = m_plan;
    const bool sent =
        m_read.start([hub, &request](const ModbusSend &queue) { return hub->sendModbus(*request, nullptr, queue) > 0; },
                     how, [this, next, plan](const ModbusResult &result) { blockDone(next, plan, result); });
    if (!sent) { // no link could write it: the block went without a reply this time
        ModbusResult failed;
        failed.problem = QStringLiteral("发送失败");
        blockDone(next, plan, failed);
    }
}

void ModbusPoller::replan()
{
    const Settings current = settings();
    QList<ModbusNeed> active;
    for (ModbusNeed need : m_context->modbusNeeds()) {
        if (!need.visible) {
            continue; // on a tab page in the background, or hidden
        }
        if (need.slave <= 0) {
            need.slave = current.fallbackSlave;
        }
        if (need.slave < 1 || need.slave > 247 || need.address < 0 || need.address > 65535) {
            continue; // no request can read it
        }
        need.period = std::max(0, need.period);
        active.append(need);
    }
    const QList<ModbusBlock> blocks = planBlocks(active, current.defaultPeriod);
    if (blocks == m_blocks) {
        if (blocks.isEmpty()) {
            setStatus(tr("没有 Modbus 绑定"));
        }
        return;
    }
    // A new plan starts a new round. A block that stays keeps its time; a new one is read right away.
    QList<qint64> due(blocks.size(), 0);
    for (int i = 0; i < int(blocks.size()); ++i) {
        const qsizetype old = m_blocks.indexOf(blocks.at(i));
        if (old >= 0) {
            due[i] = m_due.at(old);
        }
    }
    m_blocks = blocks;
    m_due = due;
    m_results = QList<std::optional<ModbusResult>>(blocks.size());
    ++m_plan;
    setStatus(blocks.isEmpty() ? tr("没有 Modbus 绑定") : tr("%1 块 · 轮询中").arg(blocks.size()));
}

void ModbusPoller::updateInterval()
{
    // Every block is looked at as often as the fastest of them, and the default period, needs.
    int interval = settings().defaultPeriod;
    for (const ModbusBlock &block : std::as_const(m_blocks)) {
        interval = std::min(interval, block.period);
    }
    interval = std::max(1, interval);
    if (m_timer->interval() != interval) {
        m_timer->setInterval(interval);
    }
}

void ModbusPoller::blockDone(int block, int plan, const ModbusResult &result)
{
    if (!m_active || m_paused || result.cancelled) {
        return; // a link that went down: a fresh plan follows once one can send again
    }
    if (plan == m_plan && block < int(m_results.size())) {
        std::optional<ModbusResult> &slot = m_results[block];
        if (!slot || slot->ok) { // a block that failed once in a round stays failed for that round
            slot = result;
        }
        if (std::all_of(m_results.cbegin(), m_results.cend(),
                        [](const std::optional<ModbusResult> &r) { return r.has_value(); })) {
            roundDone();
        }
    }
    poll(); // the next due block goes out right away
}

void ModbusPoller::roundDone()
{
    const int count = int(m_blocks.size());
    QString status = tr("%1 块 · 正常").arg(count);
    for (int k = 0; k < count; ++k) {
        const ModbusResult &result = *m_results.at(k);
        if (result.ok) {
            continue;
        }
        const ModbusBlock &block = m_blocks.at(k);
        const QString exception = modbusException(result.message);
        status = exception.isEmpty() ? tr("第 %1 块无响应（从站 %2 功能码 %3 地址 %4）")
                                           .arg(k + 1)
                                           .arg(block.slave)
                                           .arg(block.fc)
                                           .arg(block.start)
                                     : tr("第 %1 块%2").arg(k + 1).arg(exception);
        break;
    }
    m_results = QList<std::optional<ModbusResult>>(count);
    setStatus(status);
}

void ModbusPoller::setStatus(const QString &text)
{
    QStringList lines;
    for (const ModbusBlock &block : std::as_const(m_blocks)) {
        lines << tr("从站 %1 · 功能码 %2 · 地址 %3–%4 · 周期 %5 ms")
                     .arg(block.slave)
                     .arg(block.fc)
                     .arg(block.start)
                     .arg(block.start + block.count - 1)
                     .arg(block.period);
    }
    const QString tip = lines.isEmpty() ? text : lines.join(QLatin1Char('\n'));
    if (text == m_status && tip == m_toolTip) {
        return;
    }
    m_status = text;
    m_toolTip = tip;
    emit statusChanged(m_status, m_toolTip);
}

} // namespace fhmi
