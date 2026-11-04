#pragma once

#include "core/command_encoder.h"
#include "core/message.h"
#include "core/protocol_parser.h"

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPair>
#include <QtCore/QString>

namespace fhmi {

// The role a serial, TCP or UDP card plays on Modbus (modbusRole).
namespace ModbusRole {
inline const QString Master = QStringLiteral("主站"); // i18n: value
inline const QString Slave = QStringLiteral("从站");  // i18n: value
} // namespace ModbusRole

// The data table of a Modbus slave: coils, discrete inputs, holding registers and input registers ("co", "di",
// "hr", "ir"), each a run of addresses. Registers hold 0–65535, coils and discrete inputs 0 or 1.
class ModbusSlaveStore
{
  public:
    struct Area
    {
        int start = 0;
        int count = 0;
    };

    ModbusSlaveStore() = default;
    // Each table's area (start clamped to 0–65535, count to 0–10000 and to 65536 − start) filled with 0, then the
    // lines of `init`: tableAddress=value (hr100=1234, co3=1), the table in any case, blanks around the parts
    // allowed. Registers take −32768–65535, a negative value stored as its 16-bit two's complement; coils and
    // discrete inputs 0 or 1. A blank line, a # comment, a malformed line, a value out of range or an address
    // outside the area is skipped; a later line wins.
    ModbusSlaveStore(const QHash<QString, Area> &areas, const QString &init);
    // From a link card's slaveCoStart … slaveIrCount and slaveInit; a missing property takes its default.
    static ModbusSlaveStore fromProperties(const QJsonObject &props);

    // co, di, hr, ir: the order the start messages come in.
    static const QStringList &tables();
    // The function code that reads `table`: co 1, di 2, hr 3, ir 4.
    static int readFunction(const QString &table);

    Area area(const QString &table) const;
    // Whether `count` (at least 1) addresses from `address` on are all in the table's area.
    bool contains(const QString &table, int address, int count = 1) const;
    // 0 outside the area.
    quint16 value(const QString &table, int address) const;
    // Writes from `address` on; the caller checked contains().
    void write(const QString &table, int address, const QList<quint16> &values);

    // The slave message after a write to `addresses`: those addresses and each one's neighbours in the area,
    // with the values now, after "slave" and "fc" (the table's read function code).
    Message changeMessage(int slave, const QString &table, const QList<int> &addresses) const;
    // One message per table with addresses, holding all of them: what the bindings start from.
    QList<Message> startMessages(int slave) const;

  private:
    struct Table
    {
        Area area;
        QList<quint16> values;
    };
    static int indexOf(const QString &table);

    Table m_tables[4];
};

// How a slave answered one request (answerModbusRequest()).
struct ModbusSlaveAnswer
{
    bool handled = false;               // false: for another slave, ignored without a log line
    bool failed = false;                // an exception, whether or not it was answered
    QList<QPair<QString, QString>> log; // [text, level] lines logged on receipt
    QByteArray reply;                   // empty: no reply
    QPair<QString, QString> replyLog;   // the line logged when the reply goes out
    QList<Message> emits;               // the slave messages of what it wrote
};

enum class ModbusFraming
{
    Rtu, // parser modbus: slave, PDU, CRC
    Tcp, // parser modbus_tcp: MBAP header, PDU
};

// Answers one request as slave `slave`: a whole RTU frame whose CRC is good, or a whole TCP ADU. RTU takes its
// own address, and 0 as a broadcast (writes done, nothing answered); TCP takes its own unit id, 0 and 255. FC 01–06,
// 0F and 10 are supported; a write takes effect in `store` at once. With `exceptions` off a request in error gets
// no reply. The log lines read 收 <request HEX> · <what it asks> and 发 <reply HEX> · 正常 (or the exception).
ModbusSlaveAnswer answerModbusRequest(ModbusSlaveStore &store, ModbusFraming framing, int slave, bool exceptions,
                                      const QByteArray &frame);

// Frames the requests a Modbus RTU master sends: FC 01–06 take 8 bytes, 0F and 10 9 + their byte count, CRC
// checked, resynchronising on noise. A request of another function code ends at a silence (50 ms), when the
// buffer is one whole frame of at least 4 bytes; frames() leaves it for flush() unless a whole known request
// follows it. parse() gives no messages: the slave answers each frame.
class ModbusRtuRequestFramer : public Parser
{
  public:
    QList<QByteArray> frames(const QByteArray &data) override;
    QList<QByteArray> flush() override;
    int idleTimeout() const override { return 50; }
    QList<Message> parse(const QByteArray &) override { return {}; }
    void reset() override { m_buf.clear(); }

  private:
    // The length of the request at `from` when its function code fixes it: 0 for another code, -1 while too few
    // bytes are in to tell.
    int knownLength(int from) const;
    bool wholeAt(int from) const;

    QByteArray m_buf;
};

// Frames Modbus TCP requests by their MBAP header; a header with a protocol id other than 0 or a length outside
// 2–254 drops the buffer.
class ModbusTcpRequestFramer : public Parser
{
  public:
    QList<QByteArray> frames(const QByteArray &data) override;
    QList<Message> parse(const QByteArray &) override { return {}; }
    void reset() override { m_buf.clear(); }

  private:
    QByteArray m_buf;
};

// The slave of one serial, TCP or UDP card whose modbusRole is 从站. Its table lives as long as the running project,
// whichever link the card has; the card's link answers through it while it parses modbus or modbus_tcp
// (Link::setModbusSlave()). Every write, from a master, a control, a command or the register table, reports the
// change as a slave message, which the hub hands on like a received one (CommHub::addModbusSlave()).
class ModbusSlave : public QObject
{
    Q_OBJECT

  public:
    struct Settings
    {
        int slave = 1;          // modbusSlave
        int delayMs = 0;        // modbusDelay: how long after a request its reply goes out
        bool exceptions = true; // modbusExceptions
    };

    // From a link card's modbusSlave, modbusDelay, modbusExceptions and table properties.
    static Settings settingsFromProperties(const QJsonObject &props);

    ModbusSlave(const Settings &settings, ModbusSlaveStore store, QObject *parent = nullptr);

    const Settings &settings() const { return m_settings; }
    const ModbusSlaveStore &store() const { return m_store; }

    // A request a link framed: answered now, its writes made and reported, its receipt logged. The link sends
    // the reply, if any, and then calls replied().
    ModbusSlaveAnswer request(ModbusFraming framing, const QByteArray &frame);
    void replied(const ModbusSlaveAnswer &answer);

    // Writes `values` from `address` on and reports the change; false (nothing written) unless the addresses are
    // all in the area.
    bool write(const QString &table, int address, const QList<quint16> &values);
    // A control's own value at its Modbus binding: a coil or discrete input takes 1 for any value but 0; registers
    // take the value as `dataType` in `regOrder`, clamped to the type's range, the way a Modbus command packs it.
    bool writeValue(const QString &table, int address, double value, const QString &dataType, const QString &regOrder);
    // A Modbus command: FC 05 writes the coil (1 for any value but 0), FC 06 and 16 the registers; reads do nothing.
    bool execute(const ModbusRequest &request);

    QList<Message> startMessages() const { return m_store.startMessages(m_settings.slave); }

    // 从站 S · 应答 N, then · 异常 M once a request was in error; counted since resetCounts().
    QString status() const;
    void resetCounts();

  signals:
    void changed(const fhmi::Message &message);
    void logged(const QString &text, const QString &level);
    void statusChanged();

  private:
    Settings m_settings;
    ModbusSlaveStore m_store;
    int m_replies = 0;
    int m_errors = 0;
};

} // namespace fhmi
