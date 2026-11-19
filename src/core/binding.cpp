#include "core/binding.h"

#include "core/expression.h"
#include "core/hex_utils.h"
#include "core/industrial_catalog.h"
#include "core/widget_meta.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QtEndian>

#include <algorithm>
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

std::optional<CanSignalDef> canBitField(const QJsonObject &binding, QString *error)
{
    CanSignalDef signal;
    signal.start = binding.value(QStringLiteral("canStartBit")).toInt(0);
    signal.length = binding.value(QStringLiteral("canBitLength")).toInt(0);
    signal.name = QStringLiteral("%1 %2|%3").arg(binding.value(QStringLiteral("canId")).toString())
                      .arg(signal.start).arg(signal.length).trimmed();
    const QString order = binding.value(QStringLiteral("canByteOrder")).toString(CanByteOrder::Intel);
    if (order == CanByteOrder::MotorolaLsb) {
        const auto msb = motorolaMsbStart(signal.start, signal.length);
        if (!msb) {
            if (error) {
                // clang-format off
                *error = QCoreApplication::translate("fhmi", "按 Motorola LSB，起始位 %1、长度 %2 的信号会超出报文开头（第 0 字节之前）")
                             .arg(signal.start)
                             .arg(signal.length);
                // clang-format on
            }
            return std::nullopt;
        }
        signal.start = *msb;
        signal.motorola = true;
    } else if (order == CanByteOrder::MotorolaMsb) {
        signal.motorola = true;
    } else if (order != CanByteOrder::Intel) {
        // A misspelt order would read the wrong bits.
        if (error) {
            *error = QCoreApplication::translate("fhmi", "未知的字节序: %1").arg(order);
        }
        return std::nullopt;
    }
    signal.isSigned = binding.value(QStringLiteral("canSigned")).toBool(false);
    signal.factor = binding.value(QStringLiteral("canFactor")).toDouble(1);
    signal.offset = binding.value(QStringLiteral("canOffset")).toDouble(0);
    if (!signal.validate(error)) {
        return std::nullopt;
    }
    return signal;
}

namespace {

std::optional<std::pair<CanId, CanSignalDef>> canSignalOf(const QJsonObject &binding, QString *error = nullptr)
{
    const auto id = Can::parseId(binding.value(QStringLiteral("canId")).toString(), error);
    if (id)
        if (const auto signal = canBitField(binding, error))
            return std::pair{*id, *signal};
    return std::nullopt;
}

} // namespace

QString canSignalProblem(const QJsonObject &binding)
{
    QString error;
    return canSignalOf(binding, &error) ? QString() : error;
}

QString chartCurveName(const QJsonObject &props, int curve)
{
    const QString prefix = QStringLiteral("_curve_%1_").arg(curve);
    const QString name = props.value(prefix + QLatin1String("name")).toString();
    if (!name.isEmpty())
        return name;
    if (TimingSource::usesChannels(props)) {
        const QString key = props.value(prefix + QLatin1String("key")).toString();
        const QString mode = props.value(prefix + QLatin1String("parseMode")).toString();
        if (!key.isEmpty() && mode == ParseMode::CanField)
            return key.section(QLatin1Char('.'), -1);
        if (!key.isEmpty() && mode == ParseMode::DataPoint)
            return key;
        return QCoreApplication::translate("fhmi", "通道 %1").arg(curve + 1);
    }
    return QCoreApplication::translate("fhmi", "曲线%1").arg(curve + 1);
}

QJsonObject chartCurveBinding(const QJsonObject &props, int curve)
{
    const auto field = [&props, curve](const char *name, const QJsonValue &fallback) {
        const QJsonValue value = props.value(QStringLiteral("_curve_%1_%2").arg(curve).arg(QLatin1String(name)));
        return value.isUndefined() || value.isNull() ? fallback : value;
    };
    QJsonObject binding{
        {QStringLiteral("parseMode"), field("parseMode", TimingSource::usesChannels(props) ? ParseMode::None : ParseMode::KeyValue)},
        {QStringLiteral("sourceCardId"), field("sourceCardId", QString())},
        {QStringLiteral("sourceEndpoint"), field("sourceEndpoint", QString())},
        {QStringLiteral("s7"), field("s7", QJsonObject())},
        {QStringLiteral("expression"), field("expression", QString())},
        {QStringLiteral("key"), field("key", QString())},
        {QStringLiteral("jsonPath"), field("jsonPath", QString())},
        {QStringLiteral("byteOffset"), field("byteOffset", 0)},
        {QStringLiteral("dataType"), field("dataType", QStringLiteral("float32"))},
        {QStringLiteral("byteOrder"), field("byteOrder", QStringLiteral("大端(BE)"))}, // i18n: value
        {QStringLiteral("frameHeader"), field("frameHeader", QString())},
        {QStringLiteral("frameTail"), field("frameTail", QString())},
        {QStringLiteral("mbRegIndex"), field("regIndex", 0)},
        {QStringLiteral("mbDataType"), field("mbDataType", QStringLiteral("uint16"))},
        {QStringLiteral("mbRegOrder"), field("mbRegOrder", QStringLiteral("大端(AB CD)"))}, // i18n: value
        {QStringLiteral("mbFcFilter"), field("mbFcFilter", -1)},
        {QStringLiteral("mbSlaveFilter"), field("mbSlaveFilter", -1)},
        {QStringLiteral("mbPeriod"), field("mbPeriod", 0)},
        {QStringLiteral("mbArea"), field("mbArea", ModbusArea::HoldingRegisters)},
        {QStringLiteral("canId"), field("canId", QString())},
        {QStringLiteral("canStartBit"), field("canStartBit", 0)},
        {QStringLiteral("canBitLength"), field("canBitLength", 8)},
        {QStringLiteral("canByteOrder"), field("canByteOrder", CanByteOrder::Intel)},
        {QStringLiteral("canSigned"), field("canSigned", false)},
    };
    // Factor and offset are left out when unset (Undefined removes the key), so the signal's 1 and 0 apply.
    binding.insert(QStringLiteral("canFactor"), field("canFactor", QJsonValue(QJsonValue::Undefined)));
    binding.insert(QStringLiteral("canOffset"), field("canOffset", QJsonValue(QJsonValue::Undefined)));
    return binding;
}

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
    if (places.isEmpty() || places == QStringLiteral("自动") || value.typeId() == QMetaType::Bool) { // i18n: value
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

bool supportsBindingMode(const QString &typeName, QStringView mode)
{
    return !industrialCapabilities(typeName).structuredInput || mode == ParseMode::None ||
           mode == ParseMode::KeyValue || mode == ParseMode::JsonPath || mode == ParseMode::DataPoint;
}

std::optional<BindingEntry> BindingEntry::fromWidget(const QJsonObject &widget)
{
    const QJsonObject binding = widget.value(QStringLiteral("binding")).toObject();
    if (binding.isEmpty()) {
        return std::nullopt;
    }
    const QString mode = binding.value(QStringLiteral("parseMode")).toString(ParseMode::KeyValue);
    const QString type = widget.value(QStringLiteral("type")).toString();
    if (!supportsBindingMode(type, mode))
        return std::nullopt;
    const QString key = binding.value(QStringLiteral("key")).toString();
    const QString jsonPath = binding.value(QStringLiteral("jsonPath")).toString();
    const auto can = mode == ParseMode::CanSignal ? canSignalOf(binding) : std::nullopt;
    const bool configured =
        ((mode == ParseMode::KeyValue || mode == ParseMode::DataPoint || mode == ParseMode::CanField) && !key.isEmpty()) ||
        (mode == ParseMode::JsonPath && !jsonPath.isEmpty()) || mode == ParseMode::HexBytes ||
        mode == ParseMode::Modbus ||
        (mode == ParseMode::S7 && S7::point(binding.value(QStringLiteral("s7")).toObject()).has_value()) ||
        can.has_value();
    const QString endpoint = binding.value(QStringLiteral("sourceEndpoint")).toString();
    if (!configured || (!endpoint.isEmpty() && !allowsRouteEndpoint(bindingRouteTransport(mode)))) {
        return std::nullopt;
    }

    const WidgetMeta *meta = WidgetRegistry::instance().find(type);
    const QJsonObject props = widget.value(QStringLiteral("properties")).toObject();

    BindingEntry entry;
    entry.sourceCardId =
        mode == ParseMode::DataPoint ? QString() : binding.value(QStringLiteral("sourceCardId")).toString();
    entry.sourceEndpoint = mode == ParseMode::DataPoint ? QString() : endpoint;
    if (mode == ParseMode::S7) entry.s7 = S7::point(binding.value(QStringLiteral("s7")).toObject());
    entry.widgetId = widget.value(QStringLiteral("id")).toString();
    entry.typeName = type;
    entry.propName = meta && !meta->bindProperty().isEmpty() ? meta->bindProperty() : QStringLiteral("text");
    entry.parseMode = mode;
    entry.key = key;
    entry.jsonPath = jsonPath;
    entry.byteOffset = binding.value(QStringLiteral("byteOffset")).toInt(0);
    entry.dataType = binding.value(QStringLiteral("dataType")).toString(QStringLiteral("uint16"));
    entry.byteOrder = binding.value(QStringLiteral("byteOrder")).toString(QStringLiteral("大端(BE)")); // i18n: value
    entry.expression = binding.value(QStringLiteral("expression")).toString();
    entry.frameHeader = parseHexOrEmpty(binding.value(QStringLiteral("frameHeader")).toString());
    entry.frameTail = parseHexOrEmpty(binding.value(QStringLiteral("frameTail")).toString());
    entry.mbRegIndex = binding.value(QStringLiteral("mbRegIndex")).toInt(0);
    entry.mbSlaveFilter = binding.value(QStringLiteral("mbSlaveFilter")).toInt(-1);
    entry.mbDataType = binding.value(QStringLiteral("mbDataType")).toString(QStringLiteral("uint16"));
    entry.mbRegOrder =
        binding.value(QStringLiteral("mbRegOrder")).toString(QStringLiteral("大端(AB CD)")); // i18n: value
    entry.mbFcFilter = binding.value(QStringLiteral("mbFcFilter")).toInt(-1);
    entry.mbArea = binding.value(QStringLiteral("mbArea")).toString(ModbusArea::HoldingRegisters);
    entry.mbPeriod = binding.value(QStringLiteral("mbPeriod")).toInt(0);
    if (can) {
        entry.canId = can->first;
        entry.canSignal = can->second;
    }
    entry.displayFormat = props.value(QStringLiteral("displayFormat")).toString(QStringLiteral("自动")); // i18n: value
    entry.decimals = props.value(QStringLiteral("decimals")).toVariant().toString();
    if (entry.decimals.isEmpty()) {
        entry.decimals = QStringLiteral("自动"); // i18n: value
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
    if (parseMode == ParseMode::KeyValue || parseMode == ParseMode::DataPoint || parseMode == ParseMode::CanField) {
        return data.value(key);
    }
    if (parseMode == ParseMode::JsonPath) {
        // The first name is one of the message's fields, the rest walks into its value.
        const qsizetype dot = jsonPath.indexOf(QLatin1Char('.'));
        const QString field = dot < 0 ? jsonPath : jsonPath.left(dot);
        if (!data.contains(field)) {
            return {};
        }
        const QVariant top = data.value(field);
        return dot < 0 ? top : resolvePath(top, jsonPath.mid(dot + 1));
    }
    if (parseMode == ParseMode::Modbus) {
        return extractModbus(data);
    }
    return {};
}

QStringList ModbusArea::options()
{
    return {HoldingRegisters, QStringLiteral("输入寄存器"),      // i18n: value
            QStringLiteral("线圈"), QStringLiteral("离散输入")}; // i18n: value
}

QString ModbusArea::prefix(const QString &area)
{
    static const QHash<QString, QString> prefixes = {
        {QStringLiteral("保持寄存器"), QStringLiteral("hr")}, // i18n: value
        {QStringLiteral("输入寄存器"), QStringLiteral("ir")}, // i18n: value
        {QStringLiteral("线圈"), QStringLiteral("co")},       // i18n: value
        {QStringLiteral("离散输入"), QStringLiteral("di")}};  // i18n: value
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
    const QString table = ModbusArea::prefix(mbArea);
    if (table.isEmpty()) {
        return {};
    }
    if (table == QLatin1String("co") || table == QLatin1String("di")) {
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
    const bool bigWords = mbRegOrder == QStringLiteral("大端(AB CD)"); // i18n: value
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

const QStringList &rawBindingDataTypes()
{
    static const QStringList types{QStringLiteral("uint8"), QStringLiteral("int8"), QStringLiteral("uint16"),
        QStringLiteral("int16"), QStringLiteral("uint32"), QStringLiteral("int32"), QStringLiteral("float32"),
        QStringLiteral("ASCII")};
    return types;
}

std::optional<BindingByteRange> BindingEntry::rawByteRange(const QByteArray &raw) const
{
    if (parseMode != ParseMode::HexBytes || byteOffset < 0)
        return std::nullopt;
    qsizetype start = 0;
    qsizetype end = raw.size();
    if (!frameHeader.isEmpty()) {
        const auto index = raw.indexOf(frameHeader);
        if (index < 0)
            return std::nullopt;
        start = index + frameHeader.size();
    }
    if (!frameTail.isEmpty()) {
        end = raw.indexOf(frameTail, start);
        if (end < 0)
            return std::nullopt;
    }
    if (byteOffset > end - start)
        return std::nullopt;
    start += byteOffset;
    if (dataType == QLatin1String("ASCII")) {
        const auto terminator = raw.indexOf('\0', start);
        if (terminator >= 0)
            end = std::min(end, terminator);
        return BindingByteRange{start, end - start};
    }
    const auto size = rawBindingDataTypes().contains(dataType) ? HexOptions::fieldSize(dataType) : 0;
    if (size == 0 || size > end - start)
        return std::nullopt;
    return BindingByteRange{start, size};
}

QVariant BindingEntry::extractFromRaw(const QByteArray &raw) const
{
    const auto range = rawByteRange(raw);
    if (!range)
        return {};
    if (dataType == QLatin1String("ASCII"))
        return QString::fromLatin1(raw.constData() + range->offset, range->size);
    const auto *bytes = reinterpret_cast<const uchar *>(raw.constData() + range->offset);
    const bool be = byteOrder == QStringLiteral("大端(BE)"); // i18n: value
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

QList<QVariant> BindingEntry::extractAllFromRaw(const QByteArray &raw, quint64 stream, bool keepInvalid)
{
    QList<QVariant> values;
    auto it = m_rawFrames.find(stream);
    if (it == m_rawFrames.end()) it = m_rawFrames.insert(stream, RawFrameBuffer(frameHeader, frameTail));
    for (const QByteArray &frame : it->feed(raw)) {
        const QVariant value = extractFromRaw(frame);
        if (value.isValid() || keepInvalid) {
            values.append(value);
        }
    }
    return values;
}

QVariant BindingEntry::extractFromCan(const CanFrame &frame) const
{
    if (parseMode != ParseMode::CanSignal || frame.rtr || frame.error || frame.id != canId.id ||
        frame.extended != canId.extended) {
        return {};
    }
    return canSignal.decode(frame.data);
}

void BindingEntry::resetStream()
{
    m_rawFrames.clear();
}

void BindingEntry::resetStream(quint64 stream)
{
    m_rawFrames.remove(stream);
}

QVariant BindingEntry::resolvePath(const QVariant &root, const QString &path, bool *found)
{
    if (found) *found = false;
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
    if (found) *found = true;
    return data;
}

QVariant BindingEntry::evaluate(const QVariant &raw) const
{
    const QVariant result = evaluateStrict(raw);
    return result.isValid() ? result : raw; // Preserve the display conversion on direct bindings.
}

QVariant BindingEntry::evaluateStrict(const QVariant &raw) const
{
    if (!raw.isValid() || raw.isNull() || expression.trimmed().isEmpty()) {
        return raw;
    }
    // `value` has the raw value's kind: a bool, an int, or a float (numeric text included).
    QVariant value = raw;
    if (raw.typeId() != QMetaType::Bool && !isIntegral(raw)) {
        const auto number = toReal(raw);
        if (!number) {
            return {};
        }
        value = *number;
    }
    return Expression::evaluate(expression, value);
}

std::optional<double> BindingEntry::chartSample(const QVariant &raw) const
{
    const auto source = bindingFloat(raw);
    if (!source || expression.trimmed().isEmpty()) {
        return source;
    }
    // Preserve evaluate's bool/int/float semantics, including numeric strings. Only a valid source
    // reaches the expression: a constant expression must never turn a missing/invalid reading into a point.
    const auto result = bindingFloat(evaluate(raw));
    return result ? result : source;
}

QVariant BindingEntry::formatValue(const QVariant &value) const
{
    // Numeric inputs own their precision; display formatting must not turn an
    // integer input's feedback into a decimal string or round a decimal input twice.
    if (typeName == QLatin1String("SpinBox"))
        return value;
    if (propName != QLatin1String("text") && propName != QLatin1String("value")) {
        return value;
    }
    return formatDisplayValue(value, displayFormat, decimals);
}

QVariant formatDisplayValue(const QVariant &value, const QString &format, const QString &decimals)
{
    if (format == QStringLiteral("自动")) { // i18n: value
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
    if (format == QStringLiteral("十六进制(HEX)")) { // i18n: value
        return sign + QStringLiteral("0x") + padded(QString::number(magnitude, 16).toUpper(), 2);
    }
    if (format == QStringLiteral("二进制(BIN)")) { // i18n: value
        return sign + QStringLiteral("0b") + padded(QString::number(magnitude, 2), 8);
    }
    if (format == QStringLiteral("十进制")) { // i18n: value
        return QString::number(n);
    }
    return value;
}

} // namespace fhmi
