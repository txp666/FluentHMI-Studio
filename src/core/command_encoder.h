#pragma once
#include "core/can_frame.h"
#include "core/hex_utils.h"
#include "core/s7.h"
#include "core/data_source.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <functional>
#include <optional>

namespace fhmi {

// What a bit write does to its bit.
enum class ModbusBitAction
{
    Set,
    Clear,
    Toggle,
};

struct ModbusRequest
{
    int slave = 1;
    int func = 3;
    int address = 0;
    int value = 1; // register value for writes, quantity for reads
    QList<quint16> registers; // what an FC 16 request writes from address on
    // An FC 06 request with a bit (0–15) writes only that bit of the holding register: the master reads the
    // register with FC 03 first and writes it back with the bit changed (read-modify-write). -1 writes `value`.
    int bit = -1;
    ModbusBitAction bitAction = ModbusBitAction::Set;

    bool writesBit() const { return func == 0x06 && bit >= 0 && bit <= 15; }
    // The register value a bit write turns `current` into.
    quint16 withBit(quint16 current) const
    {
        if (bit < 0 || bit > 15)
            return current;
        const quint16 mask = quint16(1u << bit);
        switch (bitAction) {
        case ModbusBitAction::Set: return quint16(current | mask);
        case ModbusBitAction::Clear: return quint16(current & ~mask);
        case ModbusBitAction::Toggle: return quint16(current ^ mask);
        }
        return current;
    }
    ModbusDestination destination() const { return {slave, func}; }
};

// A command in CAN mode, still unresolved: what canId names depends on each CAN link's own DBC and inline signals,
// so every link resolves it itself (CanLink::prepare()).
struct CanCommand
{
    QString target;      // canId: a DBC message name, or an ID (Can::parseId)
    bool fd = false;     // canFd, for a raw ID; a DBC message takes FD from the DBC
    std::optional<int> cycleMs; // 0: send once; absent: use the DBC's GenMsgCycleTime
    QString assignments; // the template with {value} in: Signal=value, ... for a message with signals
    std::optional<QByteArray> payload; // for a raw ID: the bytes HEX mode builds from the same template
    QString payloadError;              // why there is no payload
    bool rawOnly = false;              // named byte fields must never be interpreted as DBC assignments
};

struct CommandInputValue
{
    QString name;
    QString dataType;
    QString value;
    std::optional<int> address;
};

// A command ready to send. Modbus requests stay unframed: each link frames them
// itself (MBAP for a link parsing Modbus TCP, ':' + hex + LRC for one parsing Modbus
// ASCII, CRC-terminated RTU otherwise).
struct EncodedCommand
{
    enum class Kind
    {
        S7,
        Bytes,
        Modbus,
        Can, // goes to CAN links only, as Bytes and Modbus go to the others
        CanFrame,
    };
    Kind kind = Kind::Bytes;
    fhmi::S7::Command s7;
    QByteArray bytes;
    QString textEncoding;
    QString framingNotice;
    struct FramePart
    {
        QString name;
        qsizetype size;
    };
    QList<FramePart> frames;
    ModbusRequest modbus;
    QList<CommandInputValue> inputValues;
    CanCommand can;
    fhmi::CanFrame frame;
    DataRoute route;
};

struct CommandResult
{
    std::optional<EncodedCommand> command;
    QString error; // user-facing, set when command is empty

    static CommandResult failure(const QString &message) { return {std::nullopt, message}; }
    static CommandResult ok(EncodedCommand command) { return {std::move(command), QString()}; }
};

using CommandReader = std::function<CommandResult()>;

namespace CommandEncoder {

enum class Mode { Text, Json, Hex, Can, Modbus, S7, Unknown };
Mode commandMode(QStringView value);
RouteFilter sendRouteFilter(const QJsonObject &properties, QStringView widgetType);
RouteFilter sendRouteFilter(const EncodedCommand &command);

inline const QString SendInterval = QStringLiteral("sendIntervalMs");
inline constexpr int MaxSendInterval = 60000;
bool supportsPeriodicSend(QStringView mode);
int periodicSendInterval(const QJsonObject &props);
bool supportsPeriodicInputs(const QJsonObject &props);

// Function code of a "03 读保持寄存器" style option; 0x06 when unrecognised.
int functionCode(const QString &option);
// The options in that style, in order; only those of `codes` when given.
QStringList functionOptions(const QList<int> &codes = {});

enum class ModbusArgument { Unsupported, ReadCount, Coil, Register };

struct ModbusFunctionSpec
{
    ModbusArgument argument = ModbusArgument::Unsupported;
    int maximumCount = 0;
    QStringList dataTypes;
};

struct ModbusValueRange
{
    double minimum;
    double maximum;
};

ModbusFunctionSpec modbusFunctionSpec(qint64 function);
std::optional<ModbusValueRange> modbusValueRange(QStringView dataType);

// Types a written value takes ("uint16" ... "float32") and their register orders.
QStringList dataTypes();
QStringList registerOrders();
bool usesRegisterOrder(QStringView dataType);

// The bit of a holding register an FC 06 write changes (mbBit): 0–15, or -1 for the whole register.
inline constexpr int WholeRegister = -1;
int modbusBit(const QJsonObject &props);
// mbBitAction's options: 置位 (the default), 复位, 取反.
QStringList bitActions();
ModbusBitAction bitAction(const QString &option);

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
QByteArray rtuFrame(const ModbusRequest &request);
QByteArray tcpFrame(const ModbusRequest &request, int transaction);
// ':', the RTU frame's bytes as uppercase hex with an LRC in place of the CRC, CR LF.
QByteArray asciiFrame(const ModbusRequest &request);
int nextTransaction();

CommandResult s7FromProperties(const QJsonObject &props, const std::optional<QString> &value = std::nullopt);

// A Modbus request from widget properties (mbSlave, mbFunc, mbAddress, mbValue, mbDataType,
// mbRegOrder). `value` overrides mbValue. With FC 06 and an mbBit of 0–15 it writes that bit of the register:
// mbBitAction (置位, 复位 or 取反) when given, else the value, which must be 0 or 1.
CommandResult modbusFromProperties(const QJsonObject &props, const std::optional<QString> &value = std::nullopt);
CommandResult modbusSwitchFromProperties(const QJsonObject &props, bool checked);

enum class InputListMode { None, Text, Json, Hex, Can, Registers, ModbusValue, S7Value };

struct CommandInput
{
    QString inputId;
    QString dataType;
    QString variable;
};

InputListMode inputListMode(const QJsonObject &props);
bool usesTemplate(InputListMode mode);
QString singleInputProperty(InputListMode mode);
QList<CommandInput> commandInputs(const QJsonObject &props);
bool usesFrameFields(const QJsonObject &props);
bool usesFrameSequence(const QJsonObject &props);
QStringList commandInputIds(const QJsonObject &props);
void mapInputIds(QJsonObject &properties, const std::function<QString(const QString &)> &map);
QString frameFieldConfigurationProblem(const QJsonObject &props, const QJsonObject &frameTemplates);
CommandResult frameFromTemplateDefaults(const QJsonObject &settings);
QString defaultInputVariable(qsizetype row);
bool validInputVariable(const QString &name);
QStringList hexDataTypes();

struct TemplatePart
{
    QString text;
    bool isInput = false;
};

QList<TemplatePart> templateParts(const QString &text);
QString inputToken(const QString &variable);

struct InputValue
{
    QVariant value;
    QString title;
    QString text() const;
};
using InputValueReader = std::function<std::optional<InputValue>(const QString &id)>;

struct BinaryFrameLayout
{
    struct FieldSpan
    {
        QString name;
        qsizetype offset;
        qsizetype size;
    };
    HexFrameLayout frame;
    QList<qsizetype> partSizes; // encoded body bytes for each template part, before length insertion
    QList<FieldSpan> fields; // absolute frame offsets for named fields
};

CommandResult frameFromFieldValues(const QJsonObject &props, const QJsonObject &values,
                                   const QJsonObject &frameTemplates, BinaryFrameLayout *layout = nullptr);

// The action-specific input to the encoder. Empty commands have neither a result nor an error.
CommandResult buttonFromProperties(const QJsonObject &props, const InputValueReader &inputValue = {},
                                   BinaryFrameLayout *layout = nullptr, const QJsonObject &frameTemplates = {});
CommandResult switchFromProperties(const QJsonObject &props, bool checked, const QJsonObject &frameTemplates = {});

struct SelectionOption
{
    QString label;
    QString value;

    bool operator==(const SelectionOption &other) const { return label == other.label && value == other.value; }
};

QList<SelectionOption> selectionOptions(const QJsonArray &rows);
CommandResult selectionFromProperties(const QJsonObject &props, const SelectionOption &option,
                                      const QJsonObject &frameTemplates = {});

// What a widget sends for `value` in its configured mode ("文本", "HEX", "JSON", "Modbus" or "CAN"),
// following PyQt UI Designer: the template's {value} is replaced (HEX numeric types
// pack the value first), text gets the configured newline, HEX gets header and tail.
// JSON puts the value in as JSON (itself when it is one JSON value, else a string), drops the
// whitespace and adds commandJsonNewline; a template that is not one JSON value is an error.
// CAN names its message in canId and leaves the rest to the CAN links (CanCommand).
CommandResult encode(const QJsonObject &props, const QString &value, BinaryFrameLayout *layout = nullptr,
                     const QJsonObject &frameTemplates = {});

QString modbusValueText(const ModbusRequest &request);
QString inputValuesText(const EncodedCommand &command, bool showAddresses = false);
// Display the encoded operation for the send preview and the runtime confirmation.
QString describe(const EncodedCommand &command, const QString &mode);

} // namespace CommandEncoder

} // namespace fhmi
