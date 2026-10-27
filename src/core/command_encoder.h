#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <optional>

namespace fhmi {

struct ModbusRequest
{
    int slave = 1;
    int func = 3;
    int address = 0;
    int value = 1; // register value for writes, quantity for reads
};

// A command ready to send. Modbus requests stay unframed: each link frames them
// itself (MBAP for a link parsing Modbus TCP, CRC-terminated RTU otherwise).
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

// Header + command + tail, all hex text.
std::optional<QByteArray> hexFrame(const QString &command, const QString &header, const QString &tail);

// Validated Modbus request, or an error for out-of-range fields.
std::optional<ModbusRequest> modbusRequest(qint64 slave, qint64 func, qint64 address, qint64 value,
                                           QString *error = nullptr);
QByteArray rtuFrame(const ModbusRequest &request);
QByteArray tcpFrame(const ModbusRequest &request, int transaction);
int nextTransaction();

// A Modbus request from widget properties (mbSlave, mbFunc, mbAddress, mbValue).
// `value` overrides mbValue; text must be an integer.
CommandResult modbusFromProperties(const QJsonObject &props, const std::optional<QString> &value = std::nullopt);

// What a widget sends for `value` in its configured mode ("文本", "HEX" or "Modbus"),
// following PyQt UI Designer: the template's {value} is replaced (HEX numeric types
// pack the value first), text gets the configured newline, HEX gets header and tail.
CommandResult encode(const QJsonObject &props, const QString &value);

} // namespace CommandEncoder

} // namespace fhmi
