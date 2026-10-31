#include "core/binding.h"

#include "core/expression.h"
#include "core/hex_utils.h"
#include "core/widget_meta.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

namespace fhmi {

namespace {

bool isIntegral(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

bool isReal(const QVariant &value)
{
    return value.typeId() == QMetaType::Double || value.typeId() == QMetaType::Float;
}

QString pythonFloat(double d)
{
    if (std::isnan(d)) {
        return QStringLiteral("nan");
    }
    if (std::isinf(d)) {
        return d > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    }
    if (d == std::floor(d) && std::fabs(d) < 1e16) {
        return QString::number(qint64(d)) + QStringLiteral(".0");
    }
    return QString::number(d, 'g', QLocale::FloatingPointShortest);
}

// Python float() of a value: numbers, bools and numeric text.
std::optional<double> toReal(const QVariant &value)
{
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool() ? 1.0 : 0.0;
    }
    if (isIntegral(value) || isReal(value)) {
        return value.toDouble();
    }
    if (value.typeId() == QMetaType::QString) {
        const QString text = value.toString().trimmed();
        const QString lower = text.toLower();
        if (lower == QLatin1String("nan") || lower == QLatin1String("+nan") || lower == QLatin1String("-nan")) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        if (lower == QLatin1String("inf") || lower == QLatin1String("+inf") || lower == QLatin1String("infinity")) {
            return std::numeric_limits<double>::infinity();
        }
        if (lower == QLatin1String("-inf") || lower == QLatin1String("-infinity")) {
            return -std::numeric_limits<double>::infinity();
        }
        bool ok = false;
        const double d = text.toDouble(&ok);
        if (ok) {
            return d;
        }
    }
    return std::nullopt;
}

// Python int() of a value: integers, bools, truncated floats and integer text.
std::optional<qint64> toInteger(const QVariant &value)
{
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool() ? 1 : 0;
    }
    if (isIntegral(value)) {
        return value.toLongLong();
    }
    if (isReal(value)) {
        const double d = value.toDouble();
        if (!std::isfinite(d) || std::fabs(d) >= 9.2e18) {
            return std::nullopt;
        }
        return qint64(std::trunc(d));
    }
    if (value.typeId() == QMetaType::QString) {
        bool ok = false;
        const qint64 n = value.toString().trimmed().toLongLong(&ok);
        if (ok) {
            return n;
        }
    }
    return std::nullopt;
}

template <typename T> T readValue(const uchar *bytes, bool bigEndian)
{
    return bigEndian ? qFromBigEndian<T>(bytes) : qFromLittleEndian<T>(bytes);
}

float floatFromBits(quint32 bits)
{
    float value = 0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

QString padded(const QString &digits, int width)
{
    return digits.size() >= width ? digits : QString(width - digits.size(), QLatin1Char('0')) + digits;
}

} // namespace

QString displayString(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return QStringLiteral("None");
    }
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    }
    if (isReal(value)) {
        return pythonFloat(value.toDouble());
    }
    if (value.typeId() == QMetaType::QVariantMap || value.typeId() == QMetaType::QVariantList) {
        // Nested JSON reads as compact JSON rather than as nothing.
        const QJsonValue json = QJsonValue::fromVariant(value);
        return QString::fromUtf8(json.isObject() ? QJsonDocument(json.toObject()).toJson(QJsonDocument::Compact)
                                                 : QJsonDocument(json.toArray()).toJson(QJsonDocument::Compact));
    }
    return value.toString();
}

std::optional<int> bindingInteger(const QVariant &value)
{
    const auto number = toInteger(value);
    if (!number || *number < std::numeric_limits<int>::min() || *number > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return int(*number);
}

std::optional<double> bindingFloat(const QVariant &value)
{
    const auto number = toReal(value);
    if (!number || !std::isfinite(*number)) {
        return std::nullopt;
    }
    return number;
}

std::optional<bool> bindingBoolean(const QVariant &value)
{
    if (value.typeId() == QMetaType::QString) {
        const QString token = value.toString().trimmed().toCaseFolded();
        if (token == QLatin1String("true") || token == QLatin1String("on") || token == QLatin1String("yes")) {
            return true;
        }
        if (token == QLatin1String("false") || token == QLatin1String("off") || token == QLatin1String("no")) {
            return false;
        }
    }
    const auto number = bindingFloat(value);
    if (!number) {
        return std::nullopt;
    }
    return *number != 0.0;
}

std::optional<bool> bindingLed(const QVariant &value, const QString &onValue, const QString &offValue)
{
    const QString text = displayString(value).trimmed();
    if (text == onValue) {
        return true;
    }
    if (text == offValue) {
        return false;
    }
    const auto number = bindingFloat(value);
    const auto on = bindingFloat(onValue);
    if (number && on) {
        return *number == *on;
    }
    return bindingBoolean(value);
}

QVariant bindingDecimals(const QVariant &value, const QString &places)
{
    if (places.isEmpty() || places == QStringLiteral("自动") || value.typeId() == QMetaType::Bool) {
        if (isReal(value) && std::isfinite(value.toDouble())) {
            // 15 significant digits: the noise of binary arithmetic (267 * 0.1 = 26.700000000000003)
            // goes, every digit a double really holds stays.
            return QString::number(value.toDouble(), 'g', 15).toDouble();
        }
        return value;
    }
    const auto number = toReal(value);
    bool ok = false;
    const int digits = places.toInt(&ok);
    if (!number || !ok || !std::isfinite(*number)) {
        return value;
    }
    return QString::number(*number, 'f', digits);
}

// ---------------------------------------------------------------------------
// BindingEntry
// ---------------------------------------------------------------------------

std::optional<BindingEntry> BindingEntry::fromWidget(const QJsonObject &widget)
{
    const QJsonObject binding = widget.value(QStringLiteral("binding")).toObject();
    if (binding.isEmpty()) {
        return std::nullopt;
    }
    const QString mode = binding.value(QStringLiteral("parseMode")).toString(ParseMode::KeyValue);
    const QString key = binding.value(QStringLiteral("key")).toString();
    const QString jsonPath = binding.value(QStringLiteral("jsonPath")).toString();
    const bool configured = (mode == ParseMode::KeyValue && !key.isEmpty()) ||
                            (mode == ParseMode::JsonPath && !jsonPath.isEmpty()) || mode == ParseMode::HexBytes ||
                            mode == ParseMode::Modbus;
    if (!configured) {
        return std::nullopt;
    }

    const QString type = widget.value(QStringLiteral("type")).toString();
    const WidgetMeta *meta = WidgetRegistry::instance().find(type);
    const QJsonObject props = widget.value(QStringLiteral("properties")).toObject();

    BindingEntry entry;
    entry.widgetId = widget.value(QStringLiteral("id")).toString();
    entry.typeName = type;
    entry.propName = meta && !meta->bindProperty().isEmpty() ? meta->bindProperty() : QStringLiteral("text");
    entry.parseMode = mode;
    entry.key = key;
    entry.jsonPath = jsonPath;
    entry.byteOffset = binding.value(QStringLiteral("byteOffset")).toInt(0);
    entry.dataType = binding.value(QStringLiteral("dataType")).toString(QStringLiteral("uint16"));
    entry.byteOrder = binding.value(QStringLiteral("byteOrder")).toString(QStringLiteral("大端(BE)"));
    entry.expression = binding.value(QStringLiteral("expression")).toString();
    entry.frameHeader = parseHexOrEmpty(binding.value(QStringLiteral("frameHeader")).toString());
    entry.frameTail = parseHexOrEmpty(binding.value(QStringLiteral("frameTail")).toString());
    entry.m_rawFrames = RawFrameBuffer(entry.frameHeader, entry.frameTail);
    entry.mbRegIndex = binding.value(QStringLiteral("mbRegIndex")).toInt(0);
    entry.mbSlaveFilter = binding.value(QStringLiteral("mbSlaveFilter")).toInt(-1);
    entry.mbDataType = binding.value(QStringLiteral("mbDataType")).toString(QStringLiteral("uint16"));
    entry.mbRegOrder = binding.value(QStringLiteral("mbRegOrder")).toString(QStringLiteral("大端(AB CD)"));
    entry.mbFcFilter = binding.value(QStringLiteral("mbFcFilter")).toInt(-1);
    entry.mbArea = binding.value(QStringLiteral("mbArea")).toString(ModbusArea::InResponse);
    entry.displayFormat = props.value(QStringLiteral("displayFormat")).toString(QStringLiteral("自动"));
    entry.decimals = props.value(QStringLiteral("decimals")).toVariant().toString();
    if (entry.decimals.isEmpty()) {
        entry.decimals = QStringLiteral("自动");
    }
    entry.ledOnValue = props.value(QStringLiteral("onValue")).toVariant().toString();
    entry.ledOffValue = props.value(QStringLiteral("offValue")).toVariant().toString();
    if (!props.contains(QStringLiteral("onValue"))) {
        entry.ledOnValue = QStringLiteral("1");
    }
    if (!props.contains(QStringLiteral("offValue"))) {
        entry.ledOffValue = QStringLiteral("0");
    }
    return entry;
}

QVariant BindingEntry::extractFromParsed(const Message &data) const
{
    if (parseMode == ParseMode::KeyValue) {
        return data.value(key);
    }
    if (parseMode == ParseMode::JsonPath) {
        return resolvePath(data, jsonPath);
    }
    if (parseMode == ParseMode::Modbus) {
        return extractModbus(data);
    }
    return {};
}

QStringList ModbusArea::options()
{
    return {InResponse, QStringLiteral("保持寄存器"), QStringLiteral("输入寄存器"), QStringLiteral("线圈"),
            QStringLiteral("离散输入")};
}

QString ModbusArea::prefix(const QString &area)
{
    static const QHash<QString, QString> prefixes = {{QStringLiteral("保持寄存器"), QStringLiteral("hr")},
                                                     {QStringLiteral("输入寄存器"), QStringLiteral("ir")},
                                                     {QStringLiteral("线圈"), QStringLiteral("co")},
                                                     {QStringLiteral("离散输入"), QStringLiteral("di")}};
    return prefixes.value(area);
}

QVariant BindingEntry::extractModbus(const Message &data) const
{
    if (mbSlaveFilter >= 0 && data.value(QStringLiteral("slave")).toLongLong() != mbSlaveFilter) {
        return {};
    }
    const qint64 fc = data.value(QStringLiteral("fc"), 0).toLongLong();
    if (mbFcFilter >= 0 && fc != mbFcFilter) {
        return {};
    }
    QString table = ModbusArea::prefix(mbArea);
    if (table.isEmpty()) {
        if (fc == 0x01 || fc == 0x02) {
            return data.value(QStringLiteral("coil%1").arg(mbRegIndex));
        }
        table = QStringLiteral("reg");
    } else if (table == QLatin1String("co") || table == QLatin1String("di")) {
        return data.value(table + QString::number(mbRegIndex));
    }

    const auto reg = [&data, &table](int index) -> std::optional<quint16> {
        const QVariant v = data.value(table + QString::number(index));
        if (!v.isValid()) {
            return std::nullopt;
        }
        if (!isIntegral(v) || v.toLongLong() < 0 || v.toLongLong() > 65535) {
            return std::nullopt; // Modbus register payloads must be unsigned 16-bit integers.
        }
        return quint16(v.toLongLong());
    };

    const auto r0 = reg(mbRegIndex);
    if (!r0) {
        return {};
    }
    if (mbDataType == QLatin1String("uint16")) {
        return qlonglong(*r0);
    }
    if (mbDataType == QLatin1String("int16")) {
        return qlonglong(qint16(*r0));
    }
    const auto r1 = reg(mbRegIndex + 1);
    if (!r1) {
        return {};
    }
    const bool bigWords = mbRegOrder == QStringLiteral("大端(AB CD)");
    const quint32 word = bigWords ? (quint32(*r0) << 16) | *r1 : (quint32(*r1) << 16) | *r0;
    if (mbDataType == QLatin1String("uint32")) {
        return qlonglong(word);
    }
    if (mbDataType == QLatin1String("int32")) {
        return qlonglong(qint32(word));
    }
    if (mbDataType == QLatin1String("float32")) {
        return double(floatFromBits(word));
    }
    return qlonglong(*r0);
}

std::optional<QByteArray> BindingEntry::stripFrame(const QByteArray &raw) const
{
    QByteArray data = raw;
    if (!frameHeader.isEmpty()) {
        const int idx = data.indexOf(frameHeader);
        if (idx < 0) {
            return std::nullopt;
        }
        data = data.mid(idx + frameHeader.size());
    }
    if (!frameTail.isEmpty()) {
        const int idx = data.indexOf(frameTail);
        if (idx < 0) {
            return std::nullopt;
        }
        data = data.left(idx);
    }
    return data;
}

QVariant BindingEntry::extractFromRaw(const QByteArray &raw) const
{
    if (parseMode != ParseMode::HexBytes) {
        return {};
    }
    const auto stripped = stripFrame(raw);
    if (!stripped) {
        return {};
    }
    const QByteArray &payload = *stripped;

    if (dataType == QLatin1String("ASCII")) {
        if (byteOffset < 0 || byteOffset > payload.size()) {
            return {};
        }
        int end = payload.indexOf('\0', byteOffset);
        if (end < 0) {
            end = payload.size();
        }
        return QString::fromLatin1(payload.mid(byteOffset, end - byteOffset));
    }

    static const QHash<QString, int> sizes = {
        {QStringLiteral("uint8"), 1},  {QStringLiteral("int8"), 1},  {QStringLiteral("uint16"), 2},
        {QStringLiteral("int16"), 2},  {QStringLiteral("uint32"), 4}, {QStringLiteral("int32"), 4},
        {QStringLiteral("float32"), 4}};
    const int size = sizes.value(dataType, 0);
    if (!size || byteOffset < 0 || byteOffset + size > payload.size()) {
        return {};
    }
    const auto *bytes = reinterpret_cast<const uchar *>(payload.constData() + byteOffset);
    const bool be = byteOrder == QStringLiteral("大端(BE)");
    if (dataType == QLatin1String("uint8")) {
        return qlonglong(bytes[0]);
    }
    if (dataType == QLatin1String("int8")) {
        return qlonglong(qint8(bytes[0]));
    }
    if (dataType == QLatin1String("uint16")) {
        return qlonglong(readValue<quint16>(bytes, be));
    }
    if (dataType == QLatin1String("int16")) {
        return qlonglong(readValue<qint16>(bytes, be));
    }
    if (dataType == QLatin1String("uint32")) {
        return qlonglong(readValue<quint32>(bytes, be));
    }
    if (dataType == QLatin1String("int32")) {
        return qlonglong(readValue<qint32>(bytes, be));
    }
    return double(floatFromBits(readValue<quint32>(bytes, be)));
}

QList<QVariant> BindingEntry::extractAllFromRaw(const QByteArray &raw)
{
    QList<QVariant> values;
    for (const QByteArray &frame : m_rawFrames.feed(raw)) {
        const QVariant value = extractFromRaw(frame);
        if (value.isValid()) {
            values.append(value);
        }
    }
    return values;
}

void BindingEntry::resetStream()
{
    m_rawFrames.reset();
}

QVariant BindingEntry::resolvePath(const QVariant &root, const QString &path)
{
    QVariant data = root;
    for (const QString &part : path.split(QLatin1Char('.'))) {
        if (!data.isValid() || data.isNull()) {
            return {};
        }
        if (data.typeId() == QMetaType::QVariantMap) {
            const QVariantMap map = data.toMap();
            if (!map.contains(part)) {
                return {};
            }
            data = map.value(part);
        } else if (data.typeId() == QMetaType::QVariantList) {
            const QVariantList list = data.toList();
            bool ok = false;
            int index = part.trimmed().toInt(&ok);
            if (!ok) {
                return {};
            }
            if (index < 0) {
                index += list.size(); // Python's negative indices
            }
            if (index < 0 || index >= list.size()) {
                return {};
            }
            data = list.at(index);
        } else {
            return {};
        }
    }
    return data;
}

QVariant BindingEntry::evaluate(const QVariant &raw) const
{
    if (expression.trimmed().isEmpty()) {
        return raw;
    }
    // `value` has the raw value's kind: a bool, an int, or a float (numeric text included).
    QVariant value = raw;
    if (raw.typeId() != QMetaType::Bool && !isIntegral(raw)) {
        const auto number = toReal(raw);
        if (!number) {
            return raw;
        }
        value = *number;
    }
    const QVariant result = Expression::evaluate(expression, value);
    return result.isValid() ? result : raw; // where Python's eval raises, the raw value
}

QVariant BindingEntry::formatValue(const QVariant &value) const
{
    if (propName != QLatin1String("text") && propName != QLatin1String("value")) {
        return value;
    }
    if (displayFormat == QStringLiteral("自动")) {
        return bindingDecimals(value, decimals);
    }
    std::optional<qint64> num;
    if (isIntegral(value) || value.typeId() == QMetaType::Bool) {
        num = toInteger(value);
    } else if (const auto real = toReal(value); real && std::isfinite(*real) && std::fabs(*real) < 9.2e18) {
        num = qint64(std::trunc(*real));
    }
    if (!num) {
        return value;
    }
    const qint64 n = *num;
    const quint64 magnitude = n < 0 ? quint64(-(n + 1)) + 1 : quint64(n);
    const QString sign = n < 0 ? QStringLiteral("-") : QString();
    if (displayFormat == QStringLiteral("十六进制(HEX)")) {
        return sign + QStringLiteral("0x") + padded(QString::number(magnitude, 16).toUpper(), 2);
    }
    if (displayFormat == QStringLiteral("二进制(BIN)")) {
        return sign + QStringLiteral("0b") + padded(QString::number(magnitude, 2), 8);
    }
    if (displayFormat == QStringLiteral("十进制")) {
        return QString::number(n);
    }
    return value;
}

} // namespace fhmi
