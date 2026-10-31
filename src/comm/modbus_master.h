#pragma once

#include "core/binding.h"
#include "core/command_encoder.h"
#include "core/protocol_parser.h"

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include <functional>
#include <optional>

class QTimer;

namespace fhmi {

class Link;
class ModbusMaster;

// What one bound widget or chart curve wants read, as PyQt UI Designer's modbus_needs() lists it.
struct ModbusNeed
{
    QString widgetId;
    int slave = 0; // the binding's mbSlaveFilter; <= 0 reads the Modbus card's defaultSlave
    QString table; // "hr", "ir", "co" or "di"
    int address = 0;
    int count = 1;       // registers, or bits for coils and discrete inputs
    int period = 0;      // ms; 0 uses the Modbus card's poll interval
    bool visible = true; // on screen now; a chart curve always is
};

// What a Modbus binding of a register table reads; nullopt for 响应内序号 and the other parse modes.
std::optional<ModbusNeed> modbusNeed(const BindingEntry &binding);

// One read the planner merged needs into.
struct ModbusBlock
{
    int slave = 1;
    int fc = 3;
    int start = 0;
    int count = 1;
    int period = 0;

    bool operator==(const ModbusBlock &other) const
    {
        return slave == other.slave && fc == other.fc && start == other.start && count == other.count &&
               period == other.period;
    }
    bool operator!=(const ModbusBlock &other) const { return !(*this == other); }
};

// Merges needs into read blocks, the same way as PyQt UI Designer (tests/data/poll_plan_vectors.json):
// hr reads with FC 3, ir 4, co 1, di 2; per (slave, fc), in (address, count) order, a need joins the open
// block while it starts at most 8 registers (64 bits) past the block's end and the block stays within 120
// registers (1000 bits). A block's period is the smallest of its members', 0 taken as `defaultPeriod`.
// Blocks come in (slave, fc, start) order.
QList<ModbusBlock> planBlocks(const QList<ModbusNeed> &needs, int defaultPeriod);

// "异常 02 非法数据地址" for an exception response, empty for any other message.
QString modbusException(const Message &message);

// How a queued request ended.
struct ModbusResult
{
    bool ok = false;
    Message message;               // the reply; empty without one
    QString problem;               // empty on success, 无响应（T ms 超时）, 设备返回异常 02 非法数据地址 or 发送失败
    bool timedOut = false;         // no reply in time, or the link went down first
    bool cancelled = false;        // the link went down (cancelAll), reported as timed out
    QPointer<ModbusMaster> master; // the master that sent it
};

// The Modbus master of one link: requests wait in one queue and only one is on the line at a time, so a
// write never collides with a read whose reply is still on its way. Writes go to the front of the queue,
// reads to the back. A reply belongs to the request on the line when its slave and function code match
// (an exception's fc | 0x80 too) and the start address a read names, or the address a write echoes, is
// the request's; a read reply that names no start answers some other request. Every reply still reaches
// the bindings as usual.
class ModbusMaster : public QObject
{
    Q_OBJECT

  public:
    enum class Priority
    {
        Write,
        Read,
    };
    using Done = std::function<void(const ModbusResult &)>;

    // Owned by `link`, which frames the requests as it parses replies: MBAP, ASCII or RTU.
    explicit ModbusMaster(Link *link);

    // Queues `request`; `done` runs once, on the reply, the device's exception or the timeout. Returns the
    // frame the request goes out as, empty when the link cannot take it now. `owner` tags the request for
    // cancelQueued().
    QByteArray enqueue(const ModbusRequest &request, int timeoutMs, Priority priority, Done done = {},
                       const void *owner = nullptr);
    bool busy() const { return m_onLine.has_value(); }
    int pending() const { return int(m_queue.size()); }
    // Ends every request, the one on the line included, as timed out: the link went down.
    void cancelAll();
    // Drops the requests of `owner` that have not gone out yet, without calling them back.
    void cancelQueued(const void *owner);

    // The frame `request` goes out as on a link parsing `parserType`.
    static QByteArray frame(const ModbusRequest &request, const QString &parserType);

  private:
    struct Entry
    {
        ModbusRequest request;
        QByteArray frame;
        QString framedFor; // the parser type the frame was made for
        int timeoutMs = 1000;
        Priority priority = Priority::Write;
        Done done;
        const void *owner = nullptr;
    };

    void pump();
    void onMessage(const Message &message);
    void finish(ModbusResult result);
    ModbusResult failure(bool timedOut, const QString &problem);

    Link *m_link;
    QTimer *m_timer;
    QList<Entry> m_queue;
    std::optional<Entry> m_onLine;
    bool m_pumping = false;
};

} // namespace fhmi
