#pragma once

#include "core/protocol_parser.h"

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QVariant>

#include <optional>

namespace fhmi {

// Where a Modbus binding reads: 响应内序号 counts registers within one response (reg0, coil0),
// the tables name them by register address (hr100), which the request for each response gives.
namespace ModbusArea {
inline const QString InResponse = QStringLiteral("响应内序号");
QStringList options();      // InResponse first, then 保持寄存器, 输入寄存器, 线圈, 离散输入
QString prefix(const QString &area); // "hr", "ir", "co", "di"; empty for InResponse
} // namespace ModbusArea

namespace ParseMode {
inline const QString KeyValue = QStringLiteral("键值匹配");
inline const QString JsonPath = QStringLiteral("JSON路径");
inline const QString HexBytes = QStringLiteral("HEX字节提取");
inline const QString Modbus = QStringLiteral("Modbus");
} // namespace ParseMode

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
    QString byteOrder = QStringLiteral("大端(BE)");
    QString expression;
    QByteArray frameHeader;
    QByteArray frameTail;
    int mbRegIndex = 0;
    int mbSlaveFilter = -1;
    QString mbDataType = QStringLiteral("uint16");
    QString mbRegOrder = QStringLiteral("大端(AB CD)");
    int mbFcFilter = -1;
    QString mbArea = ModbusArea::InResponse; // with a table, mbRegIndex is a register address
    QString displayFormat = QStringLiteral("自动");
    QString decimals = QStringLiteral("自动");
    QString ledOnValue = QStringLiteral("1");
    QString ledOffValue = QStringLiteral("0");

    bool usesParsedData() const { return parseMode != ParseMode::HexBytes; }

    // An invalid QVariant means "no value in this message".
    QVariant extractFromParsed(const Message &data) const;
    QVariant extractFromRaw(const QByteArray &raw) const;
    QList<QVariant> extractAllFromRaw(const QByteArray &raw);
    void resetStream();

    QVariant evaluate(const QVariant &raw) const;
    QVariant formatValue(const QVariant &value) const;

    static QVariant resolvePath(const QVariant &data, const QString &path);

  private:
    QVariant extractModbus(const Message &data) const;
    std::optional<QByteArray> stripFrame(const QByteArray &raw) const;

    RawFrameBuffer m_rawFrames;
};

} // namespace fhmi
