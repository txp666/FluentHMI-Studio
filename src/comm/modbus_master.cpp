#include "comm/modbus_master.h"

#include "comm/link.h"

#include <QtCore/QHash>
#include <QtCore/QTimer>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

namespace fhmi {

namespace {

QString timeoutText(int timeoutMs) { return QStringLiteral("无响应（%1 ms 超时）").arg(timeoutMs); }

bool isBitTable(const QString &table) { return table == QLatin1String("co") || table == QLatin1String("di"); }

} // namespace

// ---------------------------------------------------------------------------
// Needs and the block planner
// ---------------------------------------------------------------------------

std::optional<ModbusNeed> modbusNeed(const BindingEntry &binding)
{
    if (binding.parseMode != ParseMode::Modbus) {
        return std::nullopt;
    }
    const QString table = ModbusArea::prefix(binding.mbArea);
    if (table.isEmpty()) {
        return std::nullopt; // 响应内序号 names no address to read
    }
    const bool wide = binding.mbDataType == QLatin1String("uint32") || binding.mbDataType == QLatin1String("int32") ||
                      binding.mbDataType == QLatin1String("float32");
    ModbusNeed need;
    need.widgetId = binding.widgetId;
    need.slave = binding.mbSlaveFilter;
    need.table = table;
    need.address = binding.mbRegIndex;
    need.count = wide && !isBitTable(table) ? 2 : 1;
    need.period = binding.mbPeriod;
    return need;
}

QList<ModbusBlock> planBlocks(const QList<ModbusNeed> &needs, int defaultPeriod)
{
    static const QHash<QString, int> codes = {
        {QStringLiteral("hr"), 3}, {QStringLiteral("ir"), 4}, {QStringLiteral("co"), 1}, {QStringLiteral("di"), 2}};
    using Item = std::tuple<int, int, int>;                  // address, count, period
    std::map<std::pair<int, int>, std::vector<Item>> groups; // (slave, fc), in that order
    for (const ModbusNeed &need : needs) {
        const int fc = codes.value(need.table, 0);
        if (fc == 0) {
            continue;
        }
        groups[{need.slave, fc}].emplace_back(need.address, need.count, need.period != 0 ? need.period : defaultPeriod);
    }
    QList<ModbusBlock> blocks;
    for (auto &[key, items] : groups) {
        const auto [slave, fc] = key;
        const bool bits = fc == 1 || fc == 2;
        const int maxGap = bits ? 64 : 8;
        const int maxLength = bits ? 1000 : 120;
        std::sort(items.begin(), items.end());
        const auto [firstAddress, firstCount, firstPeriod] = items.front();
        ModbusBlock block{slave, fc, firstAddress, 0, firstPeriod};
        int end = firstAddress + firstCount; // exclusive
        for (size_t i = 1; i < items.size(); ++i) {
            const auto [address, count, period] = items[i];
            const int needEnd = address + count;
            if (address <= end + maxGap && needEnd - block.start <= maxLength) {
                end = std::max(end, needEnd);
                block.period = std::min(block.period, period);
                continue;
            }
            block.count = end - block.start;
            blocks.append(block);
            block = ModbusBlock{slave, fc, address, 0, period};
            end = needEnd;
        }
        block.count = end - block.start;
        blocks.append(block);
    }
    std::stable_sort(blocks.begin(), blocks.end(), [](const ModbusBlock &a, const ModbusBlock &b) {
        return std::tie(a.slave, a.fc, a.start) < std::tie(b.slave, b.fc, b.start);
    });
    return blocks;
}

QString modbusException(const Message &message)
{
    if (!message.contains(QStringLiteral("error"))) {
        return {};
    }
    static const QHash<int, QString> names = {
        {1, QStringLiteral("非法功能码")},      {2, QStringLiteral("非法数据地址")},
        {3, QStringLiteral("非法数据值")},      {4, QStringLiteral("从站设备故障")},
        {5, QStringLiteral("已确认，处理中")},  {6, QStringLiteral("从站忙")},
        {10, QStringLiteral("网关路径不可用")}, {11, QStringLiteral("网关目标设备无响应")},
    };
    const int code = message.value(QStringLiteral("error")).toInt();
    const QString hex = QStringLiteral("%1").arg(code, 2, 16, QLatin1Char('0')).toUpper();
    return QStringLiteral("异常 %1 %2").arg(hex, names.value(code)).trimmed();
}

// ---------------------------------------------------------------------------
// ModbusMaster
// ---------------------------------------------------------------------------

ModbusMaster::ModbusMaster(Link *link) : QObject(link), m_link(link), m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_onLine) {
            finish(failure(true, timeoutText(m_onLine->timeoutMs)));
        }
    });
    connect(link, &Link::messageReceived, this, &ModbusMaster::onMessage);
    connect(link, &Link::connectedChanged, this, [this](bool connected) {
        if (!connected) {
            cancelAll();
        }
    });
}

QByteArray ModbusMaster::frame(const ModbusRequest &request, const QString &parserType)
{
    if (parserType == QLatin1String("modbus_tcp")) {
        return CommandEncoder::tcpFrame(request, CommandEncoder::nextTransaction());
    }
    if (parserType == QLatin1String("modbus_ascii")) {
        return CommandEncoder::asciiFrame(request);
    }
    return CommandEncoder::rtuFrame(request);
}

QByteArray ModbusMaster::enqueue(const ModbusRequest &request, int timeoutMs, Priority priority, Done done,
                                 const void *owner)
{
    if (!m_link->canSend()) {
        return {};
    }
    const QString type = m_link->parserType();
    Entry entry{request, frame(request, type), type, std::max(1, timeoutMs), priority, std::move(done), owner};
    const QByteArray bytes = entry.frame;
    if (!m_onLine && m_queue.isEmpty() && !m_pumping) {
        // Straight onto the line: a request the link refuses to write is not accepted.
        m_onLine = std::move(entry);
        m_timer->start(m_onLine->timeoutMs);
        if (!m_link->send(bytes)) {
            m_timer->stop();
            m_onLine.reset();
            return {};
        }
        return bytes;
    }
    if (priority == Priority::Write) {
        // Ahead of every queued read; writes keep the order they were made in.
        const auto firstRead = std::find_if(m_queue.begin(), m_queue.end(),
                                            [](const Entry &queued) { return queued.priority == Priority::Read; });
        m_queue.insert(firstRead, std::move(entry));
    } else {
        m_queue.append(std::move(entry));
    }
    pump();
    return bytes;
}

void ModbusMaster::pump()
{
    if (m_pumping) {
        return;
    }
    m_pumping = true;
    const QPointer<ModbusMaster> self(this);
    while (!m_onLine && !m_queue.isEmpty()) {
        Entry entry = m_queue.takeFirst();
        if (entry.framedFor != m_link->parserType()) { // the link changed its mode while this waited
            entry.framedFor = m_link->parserType();
            entry.frame = frame(entry.request, entry.framedFor);
        }
        const QByteArray bytes = entry.frame;
        m_onLine = std::move(entry);
        m_timer->start(m_onLine->timeoutMs);
        if (m_link->send(bytes)) {
            break;
        }
        m_timer->stop();
        const Entry failed = std::move(*m_onLine);
        m_onLine.reset();
        if (failed.done) {
            failed.done(failure(false, QStringLiteral("发送失败")));
            if (!self) {
                return;
            }
        }
    }
    m_pumping = false;
}

void ModbusMaster::onMessage(const Message &message)
{
    if (!m_onLine) {
        return;
    }
    const ModbusRequest &request = m_onLine->request;
    const qint64 fc = message.value(QStringLiteral("fc")).toLongLong();
    if (!message.contains(QStringLiteral("slave")) ||
        message.value(QStringLiteral("slave")).toLongLong() != request.slave ||
        (fc != request.func && fc != (request.func | 0x80))) {
        return;
    }
    // Reads name their start address, writes echo theirs. Only an exception to a write names neither;
    // a read reply without a start (its count differs) answers some other request.
    const QVariant where = message.contains(QStringLiteral("start")) ? message.value(QStringLiteral("start"))
                                                                     : message.value(QStringLiteral("address"));
    if (where.isValid() ? where.toLongLong() != request.address : fc < 0x80) {
        return;
    }
    ModbusResult result;
    result.message = message;
    const QString exception = modbusException(message);
    result.ok = exception.isEmpty();
    if (!result.ok) {
        result.problem = QStringLiteral("设备返回") + exception;
    }
    finish(std::move(result));
}

void ModbusMaster::finish(ModbusResult result)
{
    if (!m_onLine) {
        return;
    }
    const Entry entry = std::move(*m_onLine);
    m_onLine.reset();
    m_timer->stop();
    result.master = this;
    const QPointer<ModbusMaster> self(this);
    if (entry.done) {
        entry.done(result);
    }
    if (self) {
        pump(); // the next request goes out right away, not at the next poll tick
    }
}

ModbusResult ModbusMaster::failure(bool timedOut, const QString &problem)
{
    ModbusResult result;
    result.timedOut = timedOut;
    result.problem = problem;
    result.master = this;
    return result;
}

void ModbusMaster::cancelAll()
{
    QList<Entry> entries;
    if (m_onLine) {
        entries.append(std::move(*m_onLine));
        m_onLine.reset();
    }
    entries += m_queue;
    m_queue.clear();
    m_timer->stop();
    const QPointer<ModbusMaster> self(this);
    for (const Entry &entry : std::as_const(entries)) {
        if (!entry.done) {
            continue;
        }
        ModbusResult result = failure(true, timeoutText(entry.timeoutMs));
        result.cancelled = true;
        entry.done(result);
        if (!self) {
            return;
        }
    }
}

void ModbusMaster::cancelQueued(const void *owner)
{
    m_queue.erase(
        std::remove_if(m_queue.begin(), m_queue.end(), [owner](const Entry &entry) { return entry.owner == owner; }),
        m_queue.end());
}

} // namespace fhmi
