#pragma once

#include "core/can_signal.h"
#include "core/protocol_parser.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <optional>

namespace fhmi {

// The register table a Modbus binding reads, by register address: a reply names its registers that way
// (hr100) once the request it answers is known.
namespace ModbusArea {
inline const QString HoldingRegisters = QStringLiteral("保持寄存器"); // i18n: value
QStringList options();               // 保持寄存器 (the default), 输入寄存器, 线圈, 离散输入
QString prefix(const QString &area); // "hr", "ir", "co", "di"; empty for anything else
} // namespace ModbusArea

namespace ParseMode {
inline const QString KeyValue = QStringLiteral("键值匹配");    // i18n: value
inline const QString JsonPath = QStringLiteral("JSON路径");    // i18n: value
inline const QString HexBytes = QStringLiteral("HEX字节提取"); // i18n: value
inline const QString Modbus = QStringLiteral("Modbus");
inline const QString CanSignal = QStringLiteral("CAN信号"); // i18n: value
} // namespace ParseMode

// Which bit a CAN信号 binding's canStartBit names. Fixed values in the file, not translated.
namespace CanByteOrder {
inline const QString Intel = QStringLiteral("Intel");              // the LSB, as DBC @1
inline const QString MotorolaMsb = QStringLiteral("Motorola MSB"); // the MSB in sawtooth numbering, as DBC @0
inline const QString MotorolaLsb = QStringLiteral("Motorola LSB"); // the LSB in the same numbering
inline QStringList options() { return {Intel, MotorolaMsb, MotorolaLsb}; }
} // namespace CanByteOrder

// Why a CAN信号 binding (the widget's "binding" object) reads nothing: its ID does not parse, its byte order is
// unknown, or its signal does not fit a frame. Empty when it is fine.
QString canSignalProblem(const QJsonObject &binding);

// The synthetic binding a RealtimeChart's curve describes with its _curve_{curve}_* properties, defaults filled in.
// The runtime builds the curve from it and the property panel judges it (canSignalProblem).
QJsonObject chartCurveBinding(const QJsonObject &props, int curve);

// Python str() of a decoded value, so displays read the same as in PyQt UI Designer
// (25.0 shows as "25.0", True as "True").
QString displayString(const QVariant &value);

// Conversions shared by live bindings in preview and exported applications. Each
// returns nullopt where the Python designer raises, so that one malformed field
// never stops the other bindings of the same frame.
std::optional<int> bindingInteger(const QVariant &value);  // fits Qt's signed 32-bit API
std::optional<double> bindingFloat(const QVariant &value); // finite only
std::optional<bool> bindingBoolean(const QVariant &value); // true/on/yes, false/off/no, or a number
std::optional<bool> bindingLed(const QVariant &value, const QString &onValue, const QString &offValue);
QVariant bindingDecimals(const QVariant &value, const QString &places);

// One binding: a data source in the incoming stream -> one widget property.
class BindingEntry
{
  public:
    // Reads the "binding" object of a project widget. Returns nullopt when the widget
    // has no binding, or the binding is not configured enough to match anything.
    static std::optional<BindingEntry> fromWidget(const QJsonObject &widget);

    QString widgetId;
    QString typeName;
    QString propName;
    QString parseMode = ParseMode::KeyValue;
    QString key;
    QString jsonPath;
    int byteOffset = 0;
    QString dataType = QStringLiteral("uint16");
    QString byteOrder = QStringLiteral("大端(BE)"); // i18n: value
    QString expression;
    QByteArray frameHeader;
    QByteArray frameTail;
    int mbRegIndex = 0;
    int mbSlaveFilter = -1;
    QString mbDataType = QStringLiteral("uint16");
    QString mbRegOrder = QStringLiteral("大端(AB CD)"); // i18n: value
    int mbFcFilter = -1;
    QString mbArea = ModbusArea::HoldingRegisters; // mbRegIndex is a register address in this table
    int mbPeriod = 0; // how often automatic polling reads it, in ms; 0: the default period
    QString displayFormat = QStringLiteral("自动"); // i18n: value
    QString decimals = QStringLiteral("自动");      // i18n: value
    QString ledOnValue = QStringLiteral("1");
    QString ledOffValue = QStringLiteral("0");
    // CAN信号: the frames it reads and its signal in them, built once here so a busy bus costs a lookup per
    // frame. A Motorola LSB start is already turned into the DBC (MSB) one.
    CanId canId;
    CanSignalDef canSignal;

    // Which feed a binding reads: parsed messages, the bytes of a byte link, or CAN frames (extractFromCan).
    bool usesParsedData() const { return parseMode != ParseMode::HexBytes && parseMode != ParseMode::CanSignal; }
    bool usesRawData() const { return parseMode == ParseMode::HexBytes; }

    // An invalid QVariant means "no value in this message".
    QVariant extractFromParsed(const Message &data) const;
    QVariant extractFromRaw(const QByteArray &raw) const;
    QList<QVariant> extractAllFromRaw(const QByteArray &raw);
    // A CAN信号 binding's physical value (raw × factor + offset) in a received frame; invalid for another ID, a
    // remote or error frame, or data too short for the signal.
    QVariant extractFromCan(const CanFrame &frame) const;
    void resetStream();

    QVariant evaluate(const QVariant &raw) const;
    // Charts require a finite source before applying an expression; bad results retain that source.
    std::optional<double> chartSample(const QVariant &raw) const;
    QVariant formatValue(const QVariant &value) const;

    static QVariant resolvePath(const QVariant &data, const QString &path, bool *found = nullptr);

  private:
    QVariant extractModbus(const Message &data) const;
    std::optional<QByteArray> stripFrame(const QByteArray &raw) const;

    RawFrameBuffer m_rawFrames;
};

} // namespace fhmi
