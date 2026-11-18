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

class QTimer;

namespace fhmi {

class RuntimeContext;

// Plans and schedules reads independently for every connection and destination. A slow peer cannot
// delay another connection. Each channel keeps its card's timing, fallback slave and result state.
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

    // The link cards of the project, in the order they attached: the order of the document. Each names the
    // link it owns now (none for a TCP card while it is not connected) and its modbusPeriod, modbusTimeout
    // and modbusSlave. Each connection uses its own settings, or the defaults without a card.
    // settings() without a link reports the first connected card for the aggregate view.
    void setLinkCard(const QObject *card, Link *link, const Settings &settings);
    void removeLinkCard(const QObject *card);
    Settings settings(Link *link = nullptr) const;

    // 等待连接, 没有 Modbus 绑定, N 块 · 轮询中, N 块 · 正常, 第 k 块无响应（从站 S 功能码 F 地址 A）or
    // 第 k 块异常 02 非法数据地址; empty before the runtime is active.
    QString status(const Link *link = nullptr) const;
    // No link that parses Modbus can send: the status is 等待连接, which the link cards do not show.
    bool isWaiting() const { return m_waiting; }
    // One line per block: 从站 S · 功能码 F · 地址 A–B · 周期 P ms; the status without blocks.
    QString toolTip(const Link *link = nullptr) const;
    QList<ModbusBlock> blocks() const { return m_blocks; }

    // RuntimeContext::setActive(): an active poller reads.
    void setActive(bool active);
    // RuntimeContext::clear(): a new project starts on the defaults, without link cards.
    void reset();

    // What each timer tick and each change of the links does: plan again, then send the block due first
    // independently on each free master.
    void poll();

  signals:
    void statusChanged(const QString &status, const QString &toolTip);

  private:
    struct Channel
    {
        QPointer<Link> link;
        QPointer<ModbusMaster> master;
        QString endpoint;
        Settings settings;
        QList<ModbusBlock> blocks;
        QList<qint64> due;
        QList<std::optional<ModbusResult>> results;
        QString status;
        QString tip;
        int plan = 0;
    };
    void halt();
    void replan(Channel &channel, const QList<ModbusNeed> &needs);
    void blockDone(const std::shared_ptr<Channel> &channel, int block, int plan, const ModbusResult &result);
    void describe(Channel &channel, const QString &status);
    void updateStatus();

    struct LinkCard
    {
        const QObject *card = nullptr;
        QPointer<Link> link;
        Settings settings;
    };

    RuntimeContext *m_context;
    QTimer *m_timer;
    QTimer *m_wake = nullptr; // single shot: when the block due next falls between two ticks
    QList<LinkCard> m_cards;
    bool m_active = false;
    bool m_waiting = false;
    QString m_status;
    QString m_toolTip;

    QHash<ModbusMaster *, std::shared_ptr<Channel>> m_channels;
    QList<ModbusBlock> m_blocks;
    bool m_polling = false;
};

} // namespace fhmi
