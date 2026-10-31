#pragma once

#include "comm/comm_hub.h"
#include "comm/modbus_master.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

class QTimer;

namespace fhmi {

class RuntimeContext;

// One Modbus request sent to every link that takes it at once. It completes once each of those links
// answered, with the first success, else the first failure; a link that went away counts as answered.
// Links that do not parse Modbus are sent the request without waiting, so a request only they took
// completes at once, without calling back.
class ModbusExchange
{
  public:
    using Done = std::function<void(const ModbusResult &)>;
    // Puts the request on the links, filling in how.done, how.owner and how.queuedOn.
    using Sender = std::function<bool(const ModbusSend &how)>;

    // Replaces the request on its way, if any (its result is no longer reported). False when no link took
    // the request; `done` is never called then.
    bool start(const Sender &send, ModbusSend how, Done done);
    bool isActive() const { return m_state != nullptr; }
    // Completes the request once no link it waits for is left: a link may be gone without answering.
    void settle();
    // Drops the request: reads still waiting in a queue never go out, and `done` is never called.
    void cancel();

  private:
    struct State
    {
        Done done;
        QList<QPointer<ModbusMaster>> waiting;
        QList<const ModbusMaster *> answered; // results that came in before `waiting` was known
        bool armed = false;
        bool finished = false;
        std::optional<ModbusResult> result;
    };

    void report(const std::shared_ptr<State> &state, const ModbusResult &result);
    void settle(const std::shared_ptr<State> &state);

    std::shared_ptr<State> m_state;
};

// Reads what the bindings and chart curves of a running project show, from the links that parse Modbus,
// on its own: no widget has to ask. Before each read it collects the needs again (RuntimeContext::
// modbusNeeds(), the widgets on screen and every curve) and plans them into blocks (planBlocks()). Each
// block has a time it is due; whenever the masters of those links are free, the block due first goes out
// to all of them as a read, and the next one right after its reply. A new plan starts a new round: blocks
// it keeps keep their times, new ones are read at once. It starts as soon as the runtime is active and a
// link that parses Modbus can send, and keeps going until pause().
//
// The project's first Modbus card (ModbusConfigCardWidget) sets its settings and shows its status; any
// card pauses and resumes it. Without a card it runs on the defaults and shows nothing.
class ModbusPoller : public QObject
{
    Q_OBJECT

  public:
    struct Settings
    {
        int defaultPeriod = 500; // ms, for needs without a period of their own (mbPeriod 0, curves)
        int timeoutMs = 1000;    // how long a read waits for its reply
        int fallbackSlave = 1;   // the slave of bindings whose mbSlaveFilter is <= 0

        bool operator==(const Settings &other) const
        {
            return defaultPeriod == other.defaultPeriod && timeoutMs == other.timeoutMs &&
                   fallbackSlave == other.fallbackSlave;
        }
        bool operator!=(const Settings &other) const { return !(*this == other); }
    };

    explicit ModbusPoller(RuntimeContext *context);

    // The Modbus cards that show this poller, in the order they attached: the order of the document. The
    // first one's settings apply, at once; the others only show the status. Without a panel the defaults do.
    void addPanel(const QObject *panel, const Settings &settings);
    void setSettings(const QObject *panel, const Settings &settings);
    void removePanel(const QObject *panel);
    Settings settings() const;

    // Stops reading: the reads still waiting in a queue never go out. resume() starts afresh.
    void pause();
    void resume();
    bool isPaused() const { return m_paused; }
    // Reading blocks right now: active, not paused, with a plan.
    bool isPolling() const { return m_active && !m_paused && !m_blocks.isEmpty(); }

    // 等待连接, 没有 Modbus 绑定, N 块 · 轮询中, N 块 · 正常, 第 k 块无响应（从站 S 功能码 F 地址 A）,
    // 第 k 块异常 02 非法数据地址 or 已暂停; empty before the runtime is active.
    QString status() const { return m_status; }
    // One line per block: 从站 S · 功能码 F · 地址 A–B · 周期 P ms; the status without blocks.
    QString toolTip() const { return m_toolTip; }
    QList<ModbusBlock> blocks() const { return m_blocks; }

    // RuntimeContext::setActive(): an active poller reads unless paused.
    void setActive(bool active);
    // RuntimeContext::clear(): a new project starts unpaused, on the defaults, without panels.
    void reset();

    // What each timer tick and each change of the links does: plan again, then send the block due first
    // if every master is free.
    void poll();

  signals:
    void statusChanged(const QString &status, const QString &toolTip);
    void pausedChanged(bool paused);

  private:
    void settingsChanged(const Settings &before);
    void begin();
    void halt();
    void clearPlan();
    // The connected links that parse Modbus and can send now: where blocks are read.
    QList<Link *> modbusLinks() const;
    void replan();
    void updateInterval();
    void blockDone(int block, int plan, const ModbusResult &result);
    void roundDone();
    void setStatus(const QString &text);

    RuntimeContext *m_context;
    QTimer *m_timer;
    QList<std::pair<const QObject *, Settings>> m_panels;
    bool m_active = false;
    bool m_paused = false;
    QString m_status;
    QString m_toolTip;

    ModbusExchange m_read; // the block read on its way
    QList<ModbusBlock> m_blocks;
    QList<qint64> m_due;                          // per block: when it is read next
    QList<std::optional<ModbusResult>> m_results; // per block, in this round; a failure stays for the round
    int m_plan = 0; // counts plans, so a block read under an older plan does not count in this round
};

} // namespace fhmi
