#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace fhmi {

struct ModbusRequest
{
    int slave = 1;
    int func = 3;
    int address = 0;
    int value = 1; // register value for writes, quantity for reads
    QList<quint16> registers; // what an FC 16 request writes from address on
};

// A command ready to send. Modbus requests stay unframed: each link frames them
// itself (MBAP for a link parsing Modbus TCP, ':' + hex + LRC for one parsing Modbus
// ASCII, CRC-terminated RTU otherwise).
struct EncodedCommand
{
    enum class Kind
    {
        Bytes,
        Modbus,
    };
    Kind kind = Kind::Bytes;
    QByteArray bytes;
    ModbusRequest modbus;
};

struct CommandResult
{
    std::optional<EncodedCommand> command;
    QString error; // user-facing, set when command is empty

    static CommandResult failure(const QString &message) { return {std::nullopt, message}; }
    static CommandResult ok(EncodedCommand command) { return {std::move(command), QString()}; }
};

namespace CommandEncoder {

// Function code of a "03 读保持寄存器" style option; 0x06 when unrecognised.
int functionCode(const QString &option);
// The options in that style, in order; only those of `codes` when given.
QStringList functionOptions(const QList<int> &codes = {});
// Types a written value takes ("uint16" ... "float32") and their register orders.
QStringList dataTypes();
QStringList registerOrders();

// The registers holding `value` as `dataType`: one for 16-bit types, two for 32-bit ones,
// the two swapped for 小端(CD AB).
std::optional<QList<quint16>> modbusRegisters(const QString &value, const QString &dataType,
                                              const QString &regOrder, QString *error = nullptr);

// Header + command + tail, all hex text.
std::optional<QByteArray> hexFrame(const QString &command, const QString &header, const QString &tail);

// Validated Modbus request, or an error for out-of-range fields.
std::optional<ModbusRequest> modbusRequest(qint64 slave, qint64 func, qint64 address, qint64 value,
                                           QString *error = nullptr);
// The same with the value as text: the count for reads, the coil or register value for FC 05/06
// (FC 06 also takes an int16), and for FC 16 the registers holding the typed value.
std::optional<ModbusRequest> modbusRequest(qint64 slave, qint64 func, qint64 address, const QString &value,
                                           const QString &dataType, const QString &regOrder,
                                           QString *error = nullptr);
// The reads in a Modbus card's poll table: one per line (or separated by ;) as
// 从站,功能码,地址,数量 with function codes 1–4; lines starting with # are comments.
std::optional<QList<ModbusRequest>> modbusPollTable(const QString &text, QString *error = nullptr);
QByteArray rtuFrame(const ModbusRequest &request);
QByteArray tcpFrame(const ModbusRequest &request, int transaction);
// ':', the RTU frame's bytes as uppercase hex with an LRC in place of the CRC, CR LF.
QByteArray asciiFrame(const ModbusRequest &request);
int nextTransaction();

// A Modbus request from widget properties (mbSlave, mbFunc, mbAddress, mbValue, mbDataType,
// mbRegOrder). `value` overrides mbValue.
CommandResult modbusFromProperties(const QJsonObject &props, const std::optional<QString> &value = std::nullopt);

// What a widget sends for `value` in its configured mode ("文本", "HEX" or "Modbus"),
// following PyQt UI Designer: the template's {value} is replaced (HEX numeric types
// pack the value first), text gets the configured newline, HEX gets header and tail.
CommandResult encode(const QJsonObject &props, const QString &value);

} // namespace CommandEncoder

} // namespace fhmi
