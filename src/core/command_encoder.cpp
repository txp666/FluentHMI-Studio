#include "core/command_encoder.h"

#include "core/hex_utils.h"
#include "core/protocol_parser.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace fhmi::CommandEncoder {

namespace {

QString prop(const QJsonObject &props, const char *name, const QString &fallback)
{
    const QJsonValue value = props.value(QLatin1String(name));
    return value.isUndefined() || value.isNull() ? fallback : value.toVariant().toString();
}

std::optional<qint64> integerText(const QString &text)
{
    bool ok = false;
    const qint64 n = text.trimmed().toLongLong(&ok);
    return ok ? std::optional<qint64>(n) : std::nullopt;
}

std::optional<qint64> integerProp(const QJsonObject &props, const char *name, qint64 fallback)
{
    const QJsonValue value = props.value(QLatin1String(name));
    if (value.isUndefined() || value.isNull()) {
        return fallback;
    }
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (!std::isfinite(d)) {
            return std::nullopt;
        }
        return qint64(std::trunc(d));
    }
    if (value.isBool()) {
        return value.toBool() ? 1 : 0;
    }
    return integerText(value.toString());
}

// Pack `value` as the struct type, then put its hex digits through the template.
std::optional<QByteArray> packedPayload(const QString &value, const QString &type, bool bigEndian)
{
    QByteArray out;
    if (type == QLatin1String("float32")) {
        bool ok = false;
        const double d = value.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(d) || std::fabs(d) > 3.4028234663852886e38) {
            return std::nullopt;
        }
        const float f = float(d);
        quint32 bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        out.resize(4);
        bigEndian ? qToBigEndian(bits, out.data()) : qToLittleEndian(bits, out.data());
        return out;
    }
    const auto n = integerText(value);
    if (!n) {
        return std::nullopt;
    }
    struct Range
    {
        int size;
        qint64 lo;
        qint64 hi;
    };
    static const QHash<QString, Range> ranges = {
        {QStringLiteral("uint8"), {1, 0, 0xFF}},
        {QStringLiteral("int8"), {1, -128, 127}},
        {QStringLiteral("uint16"), {2, 0, 0xFFFF}},
        {QStringLiteral("int16"), {2, -32768, 32767}},
        {QStringLiteral("uint32"), {4, 0, 0xFFFFFFFFLL}},
        {QStringLiteral("int32"), {4, -2147483648LL, 2147483647LL}},
    };
    const auto it = ranges.constFind(type);
    if (it == ranges.constEnd() || *n < it->lo || *n > it->hi) {
        return std::nullopt;
    }
    const quint32 bits = quint32(*n & 0xFFFFFFFFLL);
    out.resize(it->size);
    for (int i = 0; i < it->size; ++i) {
        const int shift = bigEndian ? 8 * (it->size - 1 - i) : 8 * i;
        out[i] = char((bits >> shift) & 0xFF);
    }
    return out;
}

QString newline(const QString &option)
{
    if (option == QStringLiteral("无")) { // i18n: value
        return QString();
    }
    if (option == QLatin1String("\\n")) {
        return QStringLiteral("\n");
    }
    if (option == QLatin1String("\\r")) {
        return QStringLiteral("\r");
    }
    return QStringLiteral("\r\n");
}

QString hexError() { return QCoreApplication::translate("fhmi", "HEX 格式错误，请检查内容、帧头和帧尾"); }

// JSON mode. Strict RFC 8259 grammar (no range checks, no NaN / Infinity), checked by hand because
// QJsonDocument neither keeps a value as written nor follows the grammar exactly.
const QString kPlaceholder = QStringLiteral("{value}");

bool jsonSpace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r';
}

class JsonValidator
{
  public:
    explicit JsonValidator(const QString &text) : m_text(text) {}

    // Exactly one JSON value, optionally surrounded by JSON whitespace.
    bool valid()
    {
        skipSpace();
        if (!value(0)) {
            return false;
        }
        skipSpace();
        return m_pos == m_text.size();
    }

  private:
    // More nested objects and arrays than this count as not JSON, as in PyQt UI Designer, rather than
    // risking the stack.
    static constexpr int kMaxDepth = 512;

    QChar peek() const { return m_pos < m_text.size() ? m_text.at(m_pos) : QChar(); }
    bool eat(QChar c)
    {
        if (m_pos >= m_text.size() || m_text.at(m_pos) != c) {
            return false;
        }
        ++m_pos;
        return true;
    }
    bool digit() const { return m_pos < m_text.size() && peek() >= u'0' && peek() <= u'9'; }
    // One or more digits.
    bool digits()
    {
        if (!digit()) {
            return false;
        }
        while (digit()) {
            ++m_pos;
        }
        return true;
    }
    void skipSpace()
    {
        while (m_pos < m_text.size() && jsonSpace(peek())) {
            ++m_pos;
        }
    }
    bool literal(const char *word)
    {
        const QLatin1String w(word);
        if (QStringView(m_text).mid(m_pos, w.size()) != w) {
            return false;
        }
        m_pos += w.size();
        return true;
    }

    bool value(int depth)
    {
        switch (peek().unicode()) {
        case u'{':
            return object(depth);
        case u'[':
            return array(depth);
        case u'"':
            return string();
        case u't':
            return literal("true");
        case u'f':
            return literal("false");
        case u'n':
            return literal("null");
        default:
            return number();
        }
    }

    bool object(int depth) // depth: the objects and arrays around this one
    {
        if (depth >= kMaxDepth) {
            return false;
        }
        ++m_pos; // '{'
        skipSpace();
        if (eat(u'}')) {
            return true;
        }
        while (true) {
            skipSpace();
            if (peek() != u'"' || !string()) {
                return false;
            }
            skipSpace();
            if (!eat(u':')) {
                return false;
            }
            skipSpace();
            if (!value(depth + 1)) {
                return false;
            }
            skipSpace();
            if (eat(u'}')) {
                return true;
            }
            if (!eat(u',')) {
                return false;
            }
        }
    }

    bool array(int depth) // depth: the objects and arrays around this one
    {
        if (depth >= kMaxDepth) {
            return false;
        }
        ++m_pos; // '['
        skipSpace();
        if (eat(u']')) {
            return true;
        }
        while (true) {
            skipSpace();
            if (!value(depth + 1)) {
                return false;
            }
            skipSpace();
            if (eat(u']')) {
                return true;
            }
            if (!eat(u',')) {
                return false;
            }
        }
    }

    bool string()
    {
        ++m_pos; // '"'
        while (m_pos < m_text.size()) {
            const QChar c = m_text.at(m_pos++);
            if (c == u'"') {
                return true;
            }
            if (c.unicode() < 0x20) {
                return false;
            }
            if (c != u'\\') {
                continue;
            }
            if (m_pos >= m_text.size()) {
                return false;
            }
            const QChar e = m_text.at(m_pos++);
            if (e == u'u') {
                for (int i = 0; i < 4; ++i) {
                    if (m_pos >= m_text.size() || !isAsciiHexDigit(m_text.at(m_pos++))) {
                        return false;
                    }
                }
            } else if (!QStringView(u"\"\\/bfnrt").contains(e)) {
                return false;
            }
        }
        return false;
    }

    static bool isAsciiHexDigit(QChar c)
    {
        return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
    }

    // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    bool number()
    {
        eat(u'-');
        if (!eat(u'0') && !digits()) {
            return false;
        }
        if (eat(u'.') && !digits()) {
            return false;
        }
        if (eat(u'e') || eat(u'E')) {
            if (!eat(u'+')) {
                eat(u'-');
            }
            if (!digits()) {
                return false;
            }
        }
        return true;
    }

    const QString &m_text;
    qsizetype m_pos = 0;
};

bool isJson(const QString &text) { return JsonValidator(text).valid(); }

// One pass over `text`: drop JSON whitespace outside strings and, when `valueJson` is given, replace each
// {value} outside strings by it; nullopt when a {value} sits inside a string literal.
std::optional<QString> fillJson(const QString &text, const std::optional<QString> &valueJson)
{
    QString out;
    out.reserve(text.size());
    bool inString = false;
    bool escaped = false;
    for (qsizetype i = 0; i < text.size();) {
        const QChar c = text.at(i);
        const bool placeholder = valueJson && QStringView(text).mid(i).startsWith(kPlaceholder);
        if (inString) {
            if (placeholder) {
                return std::nullopt;
            }
            out.append(c);
            if (escaped) {
                escaped = false;
            } else if (c == u'\\') {
                escaped = true;
            } else if (c == u'"') {
                inString = false;
            }
            ++i;
        } else if (placeholder) {
            out.append(*valueJson);
            i += kPlaceholder.size();
        } else {
            if (!jsonSpace(c)) {
                out.append(c);
                inString = c == u'"';
            }
            ++i;
        }
    }
    return out;
}

// Python's json.dumps(s, ensure_ascii=False).
QString jsonString(const QString &text)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : text) {
        switch (c.unicode()) {
        case u'"':
            out += QLatin1String("\\\"");
            break;
        case u'\\':
            out += QLatin1String("\\\\");
            break;
        case 0x08:
            out += QLatin1String("\\b");
            break;
        case 0x0C:
            out += QLatin1String("\\f");
            break;
        case 0x0A:
            out += QLatin1String("\\n");
            break;
        case 0x0D:
            out += QLatin1String("\\r");
            break;
        case 0x09:
            out += QLatin1String("\\t");
            break;
        default:
            if (c.unicode() < 0x20) {
                out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
            } else {
                out += c;
            }
        }
    }
    return out + QLatin1Char('"');
}

// Python's str.isspace(): QChar::isSpace() plus the separators U+001C..U+001F.
bool blank(const QString &text)
{
    return std::all_of(text.cbegin(), text.cend(),
                       [](QChar c) { return c.isSpace() || (c.unicode() >= 0x1C && c.unicode() <= 0x1F); });
}

CommandResult jsonCommand(const QJsonObject &props, const QString &value)
{
    const QString templ = prop(props, "commandTemplate", kPlaceholder);
    const QString effective = blank(templ) ? kPlaceholder : templ;
    const auto shape = fillJson(effective, QStringLiteral("null"));
    if (!shape || !isJson(*shape)) {
        return CommandResult::failure(QCoreApplication::translate("fhmi", "JSON 模板无效: %1").arg(templ));
    }
    const QString valueJson = isJson(value) ? *fillJson(value, std::nullopt) : jsonString(value);
    const QString option = prop(props, "commandJsonNewline", QStringLiteral("无")); // i18n: value
    const bool known = option == QLatin1String("\\r\\n") || option == QLatin1String("\\n")
                       || option == QLatin1String("\\r");
    EncodedCommand command;
    command.bytes = (*fillJson(effective, valueJson) + (known ? newline(option) : QString())).toUtf8();
    return CommandResult::ok(command);
}

} // namespace

namespace {

// Every function code option, in the order the designer lists them.
const QList<QPair<QString, int>> &functionTable()
{
    static const QList<QPair<QString, int>> table = {
        {QStringLiteral("01 读线圈"), 0x01},       {QStringLiteral("02 读离散输入"), 0x02},   // i18n: value
        {QStringLiteral("03 读保持寄存器"), 0x03}, {QStringLiteral("04 读输入寄存器"), 0x04}, // i18n: value
        {QStringLiteral("05 写单线圈"), 0x05},     {QStringLiteral("06 写单寄存器"), 0x06},   // i18n: value
        {QStringLiteral("16 写多个寄存器"), 0x10},                                            // i18n: value
    };
    return table;
}

} // namespace

int functionCode(const QString &option)
{
    for (const auto &[name, code] : functionTable()) {
        if (name == option) {
            return code;
        }
    }
    return 0x06;
}

QStringList functionOptions(const QList<int> &codes)
{
    QStringList out;
    for (const auto &[name, code] : functionTable()) {
        if (codes.isEmpty() || codes.contains(code)) {
            out.append(name);
        }
    }
    return out;
}

QStringList dataTypes()
{
    return {QStringLiteral("uint16"), QStringLiteral("int16"), QStringLiteral("uint32"), QStringLiteral("int32"),
            QStringLiteral("float32")};
}

QStringList registerOrders()
{
    return {QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")}; // i18n: value
}

std::optional<QList<quint16>> modbusRegisters(const QString &value, const QString &dataType,
                                              const QString &regOrder, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (!dataTypes().contains(dataType)) {
        return fail(QCoreApplication::translate("fhmi", "未知的 Modbus 数据类型: %1").arg(dataType));
    }
    const QString invalid = QCoreApplication::translate("fhmi", "Modbus 值无效或超出 %1 的范围").arg(dataType);
    quint32 bits = 0;
    if (dataType == QLatin1String("float32")) {
        bool ok = false;
        const double d = value.trimmed().toDouble(&ok);
        // Below this, a double rounds to a finite float; converting one beyond it is undefined.
        if (!ok || !std::isfinite(d) || std::abs(d) >= 0x1.ffffffp+127) {
            return fail(invalid);
        }
        const float number = float(d);
        std::memcpy(&bits, &number, sizeof bits);
    } else {
        const auto number = integerText(value);
        struct Range
        {
            qint64 lo, hi;
        };
        static const QHash<QString, Range> ranges = {{QStringLiteral("uint16"), {0, 65535}},
                                                     {QStringLiteral("int16"), {-32768, 32767}},
                                                     {QStringLiteral("uint32"), {0, 4294967295LL}},
                                                     {QStringLiteral("int32"), {-2147483648LL, 2147483647LL}}};
        const Range range = ranges.value(dataType);
        if (!number || *number < range.lo || *number > range.hi) {
            return fail(invalid);
        }
        bits = quint32(*number);
    }
    if (dataType == QLatin1String("uint16") || dataType == QLatin1String("int16")) {
        return QList<quint16>{quint16(bits)};
    }
    QList<quint16> registers{quint16(bits >> 16), quint16(bits)};
    if (regOrder != registerOrders().first()) {
        std::swap(registers[0], registers[1]);
    }
    return registers;
}

std::optional<QByteArray> hexFrame(const QString &command, const QString &header, const QString &tail)
{
    const auto h = parseHex(header);
    const auto c = parseHex(command);
    const auto t = parseHex(tail);
    if (!h || !c || !t) {
        return std::nullopt;
    }
    return *h + *c + *t;
}

std::optional<ModbusRequest> modbusRequest(qint64 slave, qint64 func, qint64 address, qint64 value, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (slave < 1 || slave > 247 || address < 0 || address > 65535) {
        return fail(QCoreApplication::translate("fhmi", "Modbus 从站地址或寄存器地址超出范围"));
    }
    bool valid = false;
    if (func == 1 || func == 2) {
        valid = value >= 1 && value <= 2000;
    } else if (func == 3 || func == 4) {
        valid = value >= 1 && value <= 125;
    } else if (func == 5) {
        valid = value == 0 || value == 0xFF00;
    } else if (func == 6) {
        valid = value >= 0 && value <= 65535;
    }
    if (!valid) {
        return fail(QCoreApplication::translate("fhmi", "Modbus 值或读取数量超出功能码允许范围"));
    }
    return ModbusRequest{int(slave), int(func), int(address), int(value), {}};
}

std::optional<ModbusRequest> modbusRequest(qint64 slave, qint64 func, qint64 address, const QString &value,
                                           const QString &dataType, const QString &regOrder, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return std::nullopt;
    };
    if (func != 0x06 && func != 0x10) {
        const auto number = integerText(value);
        if (!number) {
            return fail(QCoreApplication::translate("fhmi", "Modbus 地址和值必须是整数"));
        }
        return modbusRequest(slave, func, address, *number, error);
    }
    if (slave < 1 || slave > 247 || address < 0 || address > 65535) {
        return fail(QCoreApplication::translate("fhmi", "Modbus 从站地址或寄存器地址超出范围"));
    }
    if (func == 0x06 && dataType != QLatin1String("uint16") && dataType != QLatin1String("int16")) {
        return fail(QCoreApplication::translate("fhmi", "32 位数据占两个寄存器，请用 16 写多个寄存器"));
    }
    const auto registers = modbusRegisters(value, dataType, regOrder, error);
    if (!registers) {
        return std::nullopt;
    }
    if (address + registers->size() > 65536) {
        return fail(QCoreApplication::translate("fhmi", "Modbus 从站地址或寄存器地址超出范围"));
    }
    if (func == 0x06) {
        return ModbusRequest{int(slave), 0x06, int(address), registers->first(), {}};
    }
    return ModbusRequest{int(slave), 0x10, int(address), int(registers->size()), *registers};
}

QByteArray rtuFrame(const ModbusRequest &request)
{
    return buildModbusRtuFrame(request.slave, request.func, request.address, request.value, request.value,
                               request.registers);
}

QByteArray tcpFrame(const ModbusRequest &request, int transaction)
{
    return buildModbusTcpFrame(request.slave, request.func, request.address, request.value, request.value,
                               transaction, request.registers);
}

QByteArray asciiFrame(const ModbusRequest &request)
{
    return buildModbusAsciiFrame(request.slave, request.func, request.address, request.value, request.value,
                                 request.registers);
}

int nextTransaction()
{
    static std::atomic<int> transaction{0};
    return (transaction.fetch_add(1) + 1) & 0xFFFF;
}

CommandResult modbusFromProperties(const QJsonObject &props, const std::optional<QString> &value)
{
    const auto slave = integerProp(props, "mbSlave", 1);
    const auto address = integerProp(props, "mbAddress", 0);
    const auto amount = integerProp(props, "mbValue", 1);
    if (!slave || !address || (!value && !amount)) {
        return CommandResult::failure(QCoreApplication::translate("fhmi", "Modbus 地址和值必须是整数"));
    }
    const int func = functionCode(prop(props, "mbFunc", QStringLiteral("06 写单寄存器"))); // i18n: value
    QString error;
    const auto request = modbusRequest(*slave, func, *address, value ? *value : QString::number(*amount),
                                       prop(props, "mbDataType", QStringLiteral("uint16")),
                                       prop(props, "mbRegOrder", registerOrders().first()), &error);
    if (!request) {
        return CommandResult::failure(error);
    }
    EncodedCommand command;
    command.kind = EncodedCommand::Kind::Modbus;
    command.modbus = *request;
    return CommandResult::ok(command);
}

namespace {

// HEX mode's command text: the template with {value} in, packed per dataType and byteOrder for a numeric type.
std::optional<QString> hexCommandText(const QJsonObject &props, const QString &templ, const QString &value)
{
    const QString type = prop(props, "dataType", QStringLiteral("HEX字符串")); // i18n: value
    if (type == QStringLiteral("HEX字符串")) {                                 // i18n: value
        return templ.contains(QStringLiteral("{value}")) ? QString(templ).replace(QStringLiteral("{value}"), value)
                                                          : templ;
    }
    const bool be = prop(props, "byteOrder", QStringLiteral("大端(BE)")) == QStringLiteral("大端(BE)"); // i18n: value
    const auto payload = packedPayload(value, type, be);
    if (!payload) {
        return std::nullopt;
    }
    return QString(templ).replace(QStringLiteral("{value}"), QString::fromLatin1(payload->toHex()));
}

CommandResult canCommand(const QJsonObject &props, const QString &templ, const QString &value)
{
    EncodedCommand command;
    command.kind = EncodedCommand::Kind::Can;
    CanCommand &can = command.can;
    can.target = prop(props, "canId", QString()).trimmed();
    if (can.target.isEmpty()) {
        return CommandResult::failure(QCoreApplication::translate("fhmi", "CAN 模式请填写报文名或 CAN ID"));
    }
    const QJsonValue fd = props.value(QStringLiteral("canFd"));
    can.fd = fd.isBool() ? fd.toBool() : fd.toDouble() != 0;
    can.cycleMs = int(std::clamp<qint64>(integerProp(props, "canCycle", 0).value_or(0), 0, 60000));
    can.assignments = templ.contains(QStringLiteral("{value}")) ? QString(templ).replace(QStringLiteral("{value}"), value)
                                                                : templ;
    // Whether the message has signals is up to each link; the raw bytes are ready for the links where it has none.
    if (const auto text = hexCommandText(props, templ, value)) {
        can.payload = parseHex(*text);
        if (!can.payload) {
            can.payloadError = QCoreApplication::translate("fhmi", "CAN 数据不是有效的 HEX: %1").arg(*text);
        }
    } else {
        can.payloadError = QCoreApplication::translate("fhmi", "数值无效或超出数据类型范围");
    }
    return CommandResult::ok(command);
}

} // namespace

CommandResult encode(const QJsonObject &props, const QString &value)
{
    const QString mode = prop(props, "commandMode", QStringLiteral("文本")); // i18n: value
    const QString templ = prop(props, "commandTemplate", QStringLiteral("{value}"));
    const QString header = prop(props, "frameHeader", QString());
    const QString tail = prop(props, "frameTail", QString());

    if (mode == QLatin1String("Modbus")) {
        return modbusFromProperties(props, value);
    }
    if (mode == QLatin1String("JSON")) {
        return jsonCommand(props, value);
    }
    if (mode == QLatin1String("CAN")) {
        return canCommand(props, templ, value);
    }

    EncodedCommand command;
    if (mode == QLatin1String("HEX")) {
        const auto hexText = hexCommandText(props, templ, value);
        if (!hexText) {
            return CommandResult::failure(QCoreApplication::translate("fhmi", "数值无效或超出数据类型范围"));
        }
        const auto frame = hexFrame(*hexText, header, tail);
        if (!frame) {
            return CommandResult::failure(hexError());
        }
        command.bytes = *frame;
        return CommandResult::ok(command);
    }

    QString text = templ.contains(QStringLiteral("{value}")) ? QString(templ).replace(QStringLiteral("{value}"), value)
                                                             : templ;
    text += newline(prop(props, "commandNewline", QStringLiteral("\\r\\n")));
    command.bytes = text.toUtf8();
    return CommandResult::ok(command);
}

} // namespace fhmi::CommandEncoder
