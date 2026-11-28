#include "core/command_encoder.h"
#include "core/value_text.h"
#include "core/frame_templates.h"

#include "core/binding.h"
#include "core/hex_utils.h"
#include "core/i18n.h"
#include "core/project_logic.h"
#include "core/protocol_parser.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QtEndian>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace fhmi::CommandEncoder {

Mode commandMode(QStringView value)
{
    if (value.isEmpty() || value == QStringLiteral("文本")) // i18n: value
        return Mode::Text;
    if (value == QLatin1String("JSON"))
        return Mode::Json;
    if (value == QLatin1String("HEX"))
        return Mode::Hex;
    if (value == QLatin1String("CAN"))
        return Mode::Can;
    if (value == QLatin1String("Modbus"))
        return Mode::Modbus;
    if (value == QLatin1String("S7"))
        return Mode::S7;
    return Mode::Unknown;
}

bool supportsPeriodicSend(QStringView value)
{
    const auto mode = commandMode(value);
    return mode == Mode::Text || mode == Mode::Json || mode == Mode::Hex || mode == Mode::Can;
}

int periodicSendInterval(const QJsonObject &props)
{
    const int interval = props.value(SendInterval).toInt();
    return interval > 0 && interval <= MaxSendInterval &&
                   supportsPeriodicSend(props.value(QStringLiteral("commandMode")).toString())
               ? interval : 0;
}

bool supportsPeriodicInputs(const QJsonObject &props)
{
    if (periodicSendInterval(props) == 0)
        return false;
    const auto ids = commandInputIds(props);
    return std::any_of(ids.cbegin(), ids.cend(), [](const QString &id) { return !id.isEmpty(); });
}

QString modbusValueText(const ModbusRequest &request)
{
    if (request.writesBit()) {
        switch (request.bitAction) {
        case ModbusBitAction::Set: return QCoreApplication::translate("fhmi", "位 %1 = 1").arg(request.bit);
        case ModbusBitAction::Clear: return QCoreApplication::translate("fhmi", "位 %1 = 0").arg(request.bit);
        case ModbusBitAction::Toggle: return QCoreApplication::translate("fhmi", "位 %1 取反").arg(request.bit);
        }
    }
    if (request.func == 0x05)
        return QStringLiteral("0x") + QString::number(request.value, 16).rightJustified(4, u'0').toUpper();
    if (request.func == 0x10) {
        QStringList words;
        for (quint16 value : request.registers)
            words.append(QString::number(value));
        return words.join(QStringLiteral(", "));
    }
    return QString::number(request.value);
}

QString inputValuesText(const EncodedCommand &command, bool showAddresses)
{
    QStringList lines;
    for (const auto &input : command.inputValues) {
        const QString value = QStringLiteral("%1 = %2").arg(input.name, input.value);
        if (showAddresses && input.address) {
            lines.append(QCoreApplication::translate("fhmi", "%1 (%2 · 地址 %3)")
                             .arg(value, input.dataType, QString::number(*input.address)));
        } else {
            lines.append(value);
        }
    }
    return lines.join(u'\n');
}

QString describe(const EncodedCommand &command, const QString &mode)
{
    const auto tr = [](const char *source) { return QCoreApplication::translate("fhmi", source); };
    switch (command.kind) {
    case EncodedCommand::Kind::Modbus: {
        const auto &request = command.modbus;
        const QString summary = request.writesBit()
            ? modbusValueText(request) + u'\n' + tr("先用 03 读出该寄存器，只改这一位再写回")
            : request.func != 0x10 || command.inputValues.isEmpty()
            ? (request.func < 0x05 ? tr("读取数量 %1") : tr("写入值 %1")).arg(modbusValueText(request))
            : tr("写入 %1 个寄存器").arg(request.registers.size());
        return tr("%1\n从站 %2 · 地址 %3\n%4")
            .arg(translated(functionOptions({request.func}).first())).arg(request.slave).arg(request.address)
            .arg(summary);
    }
    case EncodedCommand::Kind::S7: {
        const auto &s7 = command.s7;
        const QString value = displayString(S7::decodeValue(s7.bytes.constData(), int(s7.bytes.size()), s7.point.type));
        return tr("写入 %1 (%2)\n值 %3").arg(s7.point.address.canonical, s7.point.type.name, value);
    }
    case EncodedCommand::Kind::Can:
        return tr("CAN 报文 %1").arg(command.can.target) + u'\n' + tr("信号赋值 %1").arg(command.can.assignments);
    case EncodedCommand::Kind::CanFrame:
        return Can::frameLine(command.frame);
    case EncodedCommand::Kind::Bytes:
        if (!command.frames.isEmpty()) {
            QStringList frames;
            qsizetype offset = 0;
            for (const auto &part : command.frames) {
                frames.append(QStringLiteral("%1 · %2").arg(part.name, toHexString(command.bytes.mid(offset, part.size))));
                offset += part.size;
            }
            return frames.join(u'\n');
        }
        if (mode == QLatin1String("HEX"))
            return QString::fromLatin1(command.bytes.toHex(' ').toUpper());
        return LineParser::decode(command.bytes, command.textEncoding).replace(u'\\', QStringLiteral("\\\\"))
            .replace(u'\r', QStringLiteral("\\r")).replace(u'\n', QStringLiteral("\\n")).replace(u'\t', QStringLiteral("\\t"));
    }
    return {};
}

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

bool validModbusAddress(qint64 slave, qint64 address)
{
    return slave >= 1 && slave <= 247 && address >= 0 && address <= 65535;
}

EncodedCommand modbusCommand(const QJsonObject &props, ModbusRequest request)
{
    EncodedCommand command;
    command.route = {props.value(QStringLiteral("commandTargetId")).toString(),
                     props.value(QStringLiteral("commandEndpoint")).toString()};
    command.kind = EncodedCommand::Kind::Modbus;
    command.modbus = std::move(request);
    return command;
}

// Pack `value` as the struct type, then put its hex digits through the template.
std::optional<QByteArray> packedPayload(const QString &value, const QString &type, bool bigEndian)
{
    QByteArray out;
    if (type == QLatin1String("float64")) {
        bool ok = false;
        const double number = value.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(number))
            return std::nullopt;
        quint64 bits = 0;
        std::memcpy(&bits, &number, sizeof bits);
        out.resize(sizeof bits);
        bigEndian ? qToBigEndian(bits, out.data()) : qToLittleEndian(bits, out.data());
        return out;
    }
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
const QString kInputVariable = QStringLiteral("[A-Za-z_][A-Za-z0-9_]*");
const QString kRawHex = QStringLiteral("HEX字符串"); // i18n: value

bool bigEndian(const QJsonObject &props)
{
    return prop(props, "byteOrder", QStringLiteral("大端(BE)")) == QStringLiteral("大端(BE)"); // i18n: value
}

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

// Drop whitespace outside strings and fill named placeholders in the value positions.
std::optional<QString> fillJson(const QString &text, const QHash<QString, QString> &values = {})
{
    QString out;
    out.reserve(text.size());
    bool inString = false;
    bool escaped = false;
    const auto append = [&](const QString &literal) {
        for (const QChar c : literal) {
            if (inString) {
                out.append(c);
                if (escaped)
                    escaped = false;
                else if (c == u'\\')
                    escaped = true;
                else if (c == u'"')
                    inString = false;
            } else if (!jsonSpace(c)) {
                out.append(c);
                inString = c == u'"';
            }
        }
    };
    if (values.isEmpty()) {
        append(text);
        return out;
    }
    for (const auto &part : templateParts(text)) {
        const auto value = part.isInput ? values.constFind(part.text) : values.cend();
        if (value == values.cend()) {
            append(part.isInput ? inputToken(part.text) : part.text);
        } else {
            if (inString)
                return std::nullopt;
            out.append(*value);
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

QString jsonValue(const QString &value)
{
    return isJson(value) ? *fillJson(value) : jsonString(value);
}

CommandResult jsonCommand(const QJsonObject &props, const QHash<QString, QString> &values,
                          const std::optional<QJsonObject> &frame)
{
    const QString templ = prop(props, "commandTemplate", kPlaceholder);
    const QString effective = blank(templ) ? kPlaceholder : templ;
    QHash<QString, QString> placeholders;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        placeholders.insert(it.key(), QStringLiteral("null"));
    const auto shape = fillJson(effective, placeholders);
    if (!shape || !isJson(*shape)) {
        return CommandResult::failure(QCoreApplication::translate("fhmi", "JSON 模板无效: %1").arg(templ));
    }
    const QString option = prop(props, "commandJsonNewline", QStringLiteral("无")); // i18n: value
    const bool known = option == QLatin1String("\\r\\n") || option == QLatin1String("\\n")
                       || option == QLatin1String("\\r");
    EncodedCommand command;
    command.route = {props.value(QStringLiteral("commandTargetId")).toString(),
                     props.value(QStringLiteral("commandEndpoint")).toString()};
    const QString body = *fillJson(effective, values);
    if (frame) {
        QString error;
        const auto bytes = buildLineFrame(body, *frame, &error);
        if (!bytes)
            return CommandResult::failure(error);
        command.bytes = *bytes;
        command.textEncoding = frame->value(QStringLiteral("encoding")).toString().toLower();
    } else {
        command.bytes = (body + (known ? newline(option) : QString())).toUtf8();
    }
    return CommandResult::ok(command);
}

} // namespace

namespace {

struct ModbusFunction
{
    QString name;
    int code;
    ModbusArgument argument;
    int maximumCount = 0;
    int maximumRegisters = 0;
};

// Keep the designer's options and the encoder's constraints together.
const QList<ModbusFunction> &functionTable()
{
    static const QList<ModbusFunction> table = {
        {QStringLiteral("01 读线圈"), 0x01, ModbusArgument::ReadCount, 2000},       // i18n: value
        {QStringLiteral("02 读离散输入"), 0x02, ModbusArgument::ReadCount, 2000},   // i18n: value
        {QStringLiteral("03 读保持寄存器"), 0x03, ModbusArgument::ReadCount, 125}, // i18n: value
        {QStringLiteral("04 读输入寄存器"), 0x04, ModbusArgument::ReadCount, 125}, // i18n: value
        {QStringLiteral("05 写单线圈"), 0x05, ModbusArgument::Coil},                // i18n: value
        {QStringLiteral("06 写单寄存器"), 0x06, ModbusArgument::Register, 0, 1},    // i18n: value
        {QStringLiteral("16 写多个寄存器"), 0x10, ModbusArgument::Register, 0, 2},  // i18n: value
    };
    return table;
}

struct RegisterType
{
    QString name;
    int registerCount;
    ModbusValueRange range;
};

const QList<RegisterType> &registerTypes()
{
    // The largest double below the midpoint that would round to infinity as a float.
    static const double floatLimit = std::nextafter(0x1.ffffffp+127, 0.0);
    static const QList<RegisterType> types = {
        {QStringLiteral("uint16"), 1, {0, 65535}},
        {QStringLiteral("int16"), 1, {-32768, 32767}},
        {QStringLiteral("uint32"), 2, {0, 4294967295.0}},
        {QStringLiteral("int32"), 2, {-2147483648.0, 2147483647.0}},
        {QStringLiteral("float32"), 2, {-floatLimit, floatLimit}},
    };
    return types;
}

const RegisterType *registerType(QStringView name)
{
    for (const auto &type : registerTypes()) {
        if (type.name == name)
            return &type;
    }
    return nullptr;
}

} // namespace

int functionCode(const QString &option)
{
    for (const auto &function : functionTable()) {
        if (function.name == option) {
            return function.code;
        }
    }
    return 0x06;
}

QStringList functionOptions(const QList<int> &codes)
{
    QStringList out;
    for (const auto &function : functionTable()) {
        if (codes.isEmpty() || codes.contains(function.code)) {
            out.append(function.name);
        }
    }
    return out;
}

ModbusFunctionSpec modbusFunctionSpec(qint64 code)
{
    for (const auto &function : functionTable()) {
        if (function.code != code)
            continue;
        ModbusFunctionSpec spec{function.argument, function.maximumCount, {}};
        for (const auto &type : registerTypes()) {
            if (type.registerCount <= function.maximumRegisters)
                spec.dataTypes.append(type.name);
        }
        return spec;
    }
    return {};
}

std::optional<ModbusValueRange> modbusValueRange(QStringView dataType)
{
    const auto *type = registerType(dataType);
    return type ? std::optional(type->range) : std::nullopt;
}

QStringList dataTypes()
{
    QStringList options;
    for (const auto &type : registerTypes())
        options.append(type.name);
    return options;
}

QStringList registerOrders()
{
    return {QStringLiteral("大端(AB CD)"), QStringLiteral("小端(CD AB)")}; // i18n: value
}

bool usesRegisterOrder(QStringView dataType)
{
    const auto *type = registerType(dataType);
    return type && type->registerCount > 1;
}

int modbusBit(const QJsonObject &props)
{
    const auto bit = integerProp(props, "mbBit", WholeRegister);
    return bit && *bit >= 0 && *bit <= 15 ? int(*bit) : WholeRegister;
}

QStringList bitActions()
{
    return {QStringLiteral("置位"), QStringLiteral("复位"), QStringLiteral("取反")}; // i18n: value
}

ModbusBitAction bitAction(const QString &option)
{
    const int index = int(bitActions().indexOf(option));
    return index == 1 ? ModbusBitAction::Clear : index == 2 ? ModbusBitAction::Toggle : ModbusBitAction::Set;
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
    const auto *type = registerType(dataType);
    if (!type) {
        return fail(QCoreApplication::translate("fhmi", "未知的 Modbus 数据类型: %1").arg(dataType));
    }
    const auto &range = type->range;
    const QString invalid = QCoreApplication::translate("fhmi", "Modbus 值无效或超出 %1 的范围").arg(dataType);
    quint32 bits = 0;
    if (dataType == QLatin1String("float32")) {
        bool ok = false;
        const double d = value.trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(d) || d < range.minimum || d > range.maximum) {
            return fail(invalid);
        }
        const float number = float(d);
        std::memcpy(&bits, &number, sizeof bits);
    } else {
        const auto number = integerText(value);
        if (!number || *number < range.minimum || *number > range.maximum) {
            return fail(invalid);
        }
        bits = quint32(*number);
    }
    if (type->registerCount == 1) {
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
    if (!validModbusAddress(slave, address)) {
        return fail(QCoreApplication::translate("fhmi", "Modbus 从站地址或寄存器地址超出范围"));
    }
    const auto spec = modbusFunctionSpec(func);
    if (spec.argument == ModbusArgument::Coil) {
        if (value == 0 || value == 1 || value == 0xFF00)
            return ModbusRequest{int(slave), int(func), int(address), value == 0 ? 0 : 0xFF00, {}};
        return fail(QCoreApplication::translate("fhmi", "Modbus 线圈值应为 0 或 1"));
    }
    const bool valid = spec.argument == ModbusArgument::ReadCount
                           ? value >= 1 && value <= spec.maximumCount && address + value <= 65536
                           : func == 0x06 && value >= 0 && value <= 65535;
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
    const auto spec = modbusFunctionSpec(func);
    if (spec.argument != ModbusArgument::Register) {
        const auto number = integerText(value);
        if (!number) {
            return fail(QCoreApplication::translate("fhmi", "Modbus 地址和值必须是整数"));
        }
        return modbusRequest(slave, func, address, *number, error);
    }
    if (!validModbusAddress(slave, address)) {
        return fail(QCoreApplication::translate("fhmi", "Modbus 从站地址或寄存器地址超出范围"));
    }
    if (func == 0x06 && !spec.dataTypes.contains(dataType)) {
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

CommandResult s7FromProperties(const QJsonObject &props, const std::optional<QString> &value)
{
    if (!props.value(QStringLiteral("commandEndpoint")).toString().isEmpty())
        return CommandResult::failure(QCoreApplication::translate("fhmi", "当前模式不支持子通道"));
    const auto settings = props.value(QStringLiteral("s7Command")).toObject();
    QString error;
    const auto point = S7::point(settings, &error);
    const QString target = props.value(QStringLiteral("commandTargetId")).toString();
    if (!point) return CommandResult::failure(error);
    if (target.isEmpty()) return CommandResult::failure(QCoreApplication::translate("fhmi", "请选择 S7 目标连接"));
    if (point->address.area == S7::MemoryArea::Input)
        return CommandResult::failure(QCoreApplication::translate("fhmi", "S7 输入区 I 只读，请选择 DB、M 或 Q 写入"));
    EncodedCommand command;
    command.route = {props.value(QStringLiteral("commandTargetId")).toString(),
                     props.value(QStringLiteral("commandEndpoint")).toString()}; command.kind = EncodedCommand::Kind::S7;
    command.s7.targetId = target; command.s7.point = *point;
    const QVariant raw = value ? QVariant(*value) : settings.value(QStringLiteral("value")).isUndefined()
        ? QVariant(QStringLiteral("1")) : settings.value(QStringLiteral("value")).toVariant();
    if (!S7::encodeValue(raw, point->type, &command.s7.bytes, &error)) return CommandResult::failure(error);
    return CommandResult::ok(command);
}

CommandResult modbusFromProperties(const QJsonObject &props, const std::optional<QString> &value)
{
    const auto slave = integerProp(props, "mbSlave", 1);
    const auto address = integerProp(props, "mbAddress", 0);
    if (!slave || !address) {
        return CommandResult::failure(QCoreApplication::translate("fhmi", "Modbus 地址和值必须是整数"));
    }
    const int func = functionCode(prop(props, "mbFunc", QStringLiteral("06 写单寄存器"))); // i18n: value
    QString error;
    const auto configured = props.value(QStringLiteral("mbValue"));
    const QString amount = configured.isBool() ? QString::number(configured.toBool())
                                               : prop(props, "mbValue", QStringLiteral("1"));
    if (const int bit = modbusBit(props); func == 0x06 && bit != WholeRegister) {
        if (!validModbusAddress(*slave, *address))
            return CommandResult::failure(QCoreApplication::translate("fhmi", "Modbus 从站地址或寄存器地址超出范围"));
        ModbusRequest request{int(*slave), 0x06, int(*address), 0, {}, bit};
        if (!value && props.contains(QStringLiteral("mbBitAction"))) {
            request.bitAction = bitAction(prop(props, "mbBitAction", {}));
        } else {
            const auto number = integerText(value.value_or(amount));
            if (!number || (*number != 0 && *number != 1))
                return CommandResult::failure(QCoreApplication::translate("fhmi", "寄存器位的写入值应为 0 或 1"));
            request.bitAction = *number ? ModbusBitAction::Set : ModbusBitAction::Clear;
        }
        return CommandResult::ok(modbusCommand(props, request));
    }
    const auto request = modbusRequest(*slave, func, *address, value.value_or(amount),
                                       prop(props, "mbDataType", QStringLiteral("uint16")),
                                       prop(props, "mbRegOrder", registerOrders().first()), &error);
    if (!request) {
        return CommandResult::failure(error);
    }
    return CommandResult::ok(modbusCommand(props, *request));
}

static int modbusSwitchFunction(const QJsonObject &props)
{
    // Projects without mbArea write a coil, just like projects that select the coil explicitly.
    const QString area = prop(props, "mbArea", {});
    return area == ModbusArea::HoldingRegisters || area == ModbusArea::HoldingRegisterBit ? 0x06 : 0x05;
}

CommandResult modbusSwitchFromProperties(const QJsonObject &props, bool checked)
{
    QJsonObject write = props;
    const QString area = prop(props, "mbArea", {});
    write.insert(QStringLiteral("mbFunc"), functionOptions({modbusSwitchFunction(props)}).first());
    write.insert(QStringLiteral("mbDataType"), QStringLiteral("uint16"));
    if (area != ModbusArea::HoldingRegisterBit) {
        write.remove(QStringLiteral("mbBit"));
    } else if (!write.contains(QStringLiteral("mbBit"))) {
        write.insert(QStringLiteral("mbBit"), 0);
    }
    write.remove(QStringLiteral("mbBitAction"));
    return modbusFromProperties(write, QString::number(checked ? 1 : 0));
}

RouteFilter sendRouteFilter(const QJsonObject &props, QStringView widgetType)
{
    switch (commandMode(prop(props, "commandMode", {}))) {
    case Mode::Can: return RouteTransport::Can;
    case Mode::S7: return RouteTransport::S7;
    case Mode::Modbus:
        return ModbusDestination{int(integerProp(props, "mbSlave", 1).value_or(1)),
            widgetType == QLatin1String("ToggleSwitch") ? modbusSwitchFunction(props)
                : functionCode(prop(props, "mbFunc", widgetType == u"PushButton" ? QStringLiteral("03") : QStringLiteral("06")))};
    default: return RouteTransport::Bytes;
    }
}

RouteFilter sendRouteFilter(const EncodedCommand &command)
{
    switch (command.kind) {
    case EncodedCommand::Kind::S7: return RouteTransport::S7;
    case EncodedCommand::Kind::Can:
    case EncodedCommand::Kind::CanFrame: return RouteTransport::Can;
    case EncodedCommand::Kind::Modbus: return command.modbus.destination();
    case EncodedCommand::Kind::Bytes: return RouteTransport::Bytes;
    }
    return RouteTransport::Bytes;
}

InputListMode inputListMode(const QJsonObject &props)
{
    switch (commandMode(prop(props, "commandMode", {}))) {
    case Mode::Text: return InputListMode::Text;
    case Mode::Hex: return InputListMode::Hex;
    case Mode::Json: return InputListMode::Json;
    case Mode::Can: return InputListMode::Can;
    case Mode::Modbus:
        return functionCode(prop(props, "mbFunc", {})) == 0x10 ? InputListMode::Registers : InputListMode::ModbusValue;
    case Mode::S7: return InputListMode::S7Value;
    case Mode::Unknown: break;
    }
    return InputListMode::None;
}

bool usesTemplate(InputListMode mode)
{
    return mode == InputListMode::Text || mode == InputListMode::Json || mode == InputListMode::Hex ||
           mode == InputListMode::Can;
}

QString singleInputProperty(InputListMode mode)
{
    if (mode == InputListMode::ModbusValue)
        return QStringLiteral("mbInputId");
    if (mode == InputListMode::S7Value)
        return QStringLiteral("s7InputId");
    return {};
}

QString defaultInputVariable(qsizetype row)
{
    return QStringLiteral("p%1").arg(row + 1);
}

bool validInputVariable(const QString &name)
{
    static const QRegularExpression pattern(QRegularExpression::anchoredPattern(kInputVariable));
    return pattern.match(name).hasMatch();
}

QList<TemplatePart> templateParts(const QString &text)
{
    static const QRegularExpression token(QStringLiteral("\\{(") + kInputVariable + QStringLiteral(")\\}"));
    QList<TemplatePart> parts;
    qsizetype offset = 0;
    auto matches = token.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.capturedStart() > offset)
            parts.append({text.mid(offset, match.capturedStart() - offset)});
        parts.append({match.captured(1), true});
        offset = match.capturedEnd();
    }
    if (offset < text.size())
        parts.append({text.mid(offset)});
    return parts;
}

QString inputToken(const QString &variable)
{
    return u'{' + variable + u'}';
}

QStringList hexDataTypes()
{
    return QStringList{kRawHex, QStringLiteral("uint8"), QStringLiteral("int8")} + dataTypes();
}

namespace {

QJsonValue inputListProperty(const QJsonObject &props)
{
    if (inputListMode(props) == InputListMode::Json && !props.value(QStringLiteral("jsonUseInputs")).toBool())
        return QJsonValue(QJsonValue::Undefined);
    const auto current = props.value(QStringLiteral("commandInputs"));
    if (current.isUndefined() && inputListMode(props) == InputListMode::Registers)
        return props.value(QStringLiteral("mbWriteInputs"));
    return current;
}

} // namespace

QList<CommandInput> commandInputs(const QJsonObject &props)
{
    const auto mode = inputListMode(props);
    if (mode == InputListMode::None)
        return {};
    if (const QString key = singleInputProperty(mode); !key.isEmpty()) {
        if (mode == InputListMode::ModbusValue && functionCode(prop(props, "mbFunc", {})) == 0x06 &&
            modbusBit(props) != WholeRegister)
            return {};
        const QString id = props.value(key).toString();
        return id.isEmpty() ? QList<CommandInput>{} : QList<CommandInput>{{id, {}, {}}};
    }
    QList<CommandInput> inputs;
    for (const QJsonValue &entry : inputListProperty(props).toArray()) {
        const auto row = entry.toObject();
        const QString variable = row.value(QStringLiteral("variable")).toString().trimmed();
        inputs.append({row.value(QStringLiteral("inputId")).toString(),
                       row.value(QStringLiteral("dataType")).toString(QStringLiteral("uint16")),
                       variable.isEmpty() ? defaultInputVariable(inputs.size()) : variable});
    }
    return inputs;
}

bool usesFrameFields(const QJsonObject &props)
{
    return FrameTemplates::isBinarySend(props.value(QStringLiteral("commandMode")).toString()) &&
           props.value(FrameTemplates::SendFieldsEnabled).toBool();
}

bool usesFrameSequence(const QJsonObject &props)
{
    return props.value(QStringLiteral("commandMode")).toString() == QLatin1String("HEX") &&
           !props.value(FrameTemplates::SendSequence).toArray().isEmpty();
}

QStringList commandInputIds(const QJsonObject &props)
{
    QStringList ids;
    const auto addBindings = [&ids](const QJsonObject &frame) {
        for (const auto &entry : frame.value(FrameTemplates::SendBindings).toArray()) {
            const QString id = entry.toObject().value(QStringLiteral("inputId")).toString();
            if (!id.isEmpty() && !ids.contains(id))
                ids.append(id);
        }
    };
    if (usesFrameSequence(props)) {
        for (const auto &frame : props.value(FrameTemplates::SendSequence).toArray())
            addBindings(frame.toObject());
    } else if (usesFrameFields(props)) {
        addBindings(props);
    } else {
        for (const auto &input : commandInputs(props))
            ids.append(input.inputId);
    }
    return ids;
}

void mapInputIds(QJsonObject &properties, const std::function<QString(const QString &)> &map)
{
    const auto update = [&](QJsonObject &object, const QString &key) {
        const auto before = object.value(key).toString();
        if (before.isEmpty())
            return;
        const auto after = map(before);
        if (before != after)
            object.insert(key, after);
    };
    for (const auto &key : {QStringLiteral("s7InputId"), QStringLiteral("mbInputId")})
        update(properties, key);
    const auto mapRows = [&update](QJsonObject &object, const QString &key) {
        const auto original = object.value(key).toArray();
        auto rows = original;
        for (qsizetype row = 0; row < rows.size(); ++row) {
            const auto before = rows.at(row).toObject();
            auto input = before;
            update(input, QStringLiteral("inputId"));
            if (input != before)
                rows[row] = input;
        }
        if (rows != original)
            object.insert(key, rows);
    };
    for (const auto &key : {QStringLiteral("commandInputs"), QStringLiteral("mbWriteInputs")})
        mapRows(properties, key);
    FrameTemplates::mapSendFrames(properties, [&mapRows](QJsonObject &frame) {
        mapRows(frame, FrameTemplates::SendBindings);
    });
}

QString InputValue::text() const
{
    return value.isValid() && !value.isNull() ? dataPointCommandText(value) : QString();
}

namespace {

std::optional<CommandInputValue> readInput(const CommandInput &input, const InputValueReader &read)
{
    const auto value = read && !input.inputId.isEmpty() ? read(input.inputId) : std::nullopt;
    if (!value)
        return std::nullopt;
    return CommandInputValue{value->title.isEmpty() ? widgetIdForInput(input.inputId) : value->title, input.dataType,
                             value->text(), std::nullopt};
}

CommandResult missingInput(qsizetype row, const CommandInput &input)
{
    if (const auto name = pointNameForInput(input.inputId); !name.isEmpty())
        return CommandResult::failure(QCoreApplication::translate("fhmi", "第 %1 行的工程数据点 %2 没有有效值")
                                          .arg(row + 1).arg(name));
    return CommandResult::failure(QCoreApplication::translate("fhmi", "输入列表第 %1 行没有可用的输入框（%2）")
                                      .arg(row + 1).arg(widgetIdForInput(input.inputId)));
}

CommandResult modbusWriteFromInputs(const QJsonObject &props, const QList<CommandInput> &inputs,
                                    const InputValueReader &inputValue)
{
    const auto tr = [](const char *source) { return QCoreApplication::translate("fhmi", source); };
    const auto slave = integerProp(props, "mbSlave", 1);
    const auto address = integerProp(props, "mbAddress", 0);
    if (!slave || !address || !validModbusAddress(*slave, *address))
        return CommandResult::failure(tr("Modbus 从站地址或寄存器地址超出范围"));

    const QString order = prop(props, "mbRegOrder", registerOrders().first());
    QList<quint16> registers;
    QList<CommandInputValue> values;
    for (const auto &input : inputs) {
        auto value = readInput(input, inputValue);
        if (!value)
            return missingInput(values.size(), input);
        QString error;
        value->value = value->value.trimmed();
        const auto words = modbusRegisters(value->value, input.dataType, order, &error);
        if (!words)
            return CommandResult::failure(tr("%1 的写入值无效：%2").arg(value->name, error));
        value->address = int(*address + registers.size());
        values.append(*value);
        registers.append(*words);
        if (registers.size() > 123)
            return CommandResult::failure(tr("FC16 每帧最多写入 123 个寄存器"));
        if (*address + registers.size() > 65536)
            return CommandResult::failure(tr("Modbus 从站地址或寄存器地址超出范围"));
    }
    auto command = modbusCommand(props, {int(*slave), 0x10, int(*address), int(registers.size()), registers});
    command.inputValues = std::move(values);
    return CommandResult::ok(std::move(command));
}

} // namespace

CommandResult switchFromProperties(const QJsonObject &props, bool checked, const QJsonObject &frameTemplates)
{
    const QString mode = prop(props, "commandMode", {});
    if (mode == QLatin1String("Modbus"))
        return modbusSwitchFromProperties(props, checked);
    if (mode == QLatin1String("S7")) {
        const auto config = props.value(QStringLiteral("s7Command")).toObject();
        const auto value = config.value(QLatin1String(checked ? "onValue" : "offValue"));
        return s7FromProperties(props, value.isUndefined() ? QString::number(checked ? 1 : 0) : value.toVariant().toString());
    }
    const QString value = prop(props, checked ? "onCommand" : "offCommand", {}).trimmed();
    return value.isEmpty() ? CommandResult{} : encode(props, value, nullptr, frameTemplates);
}

QList<SelectionOption> selectionOptions(const QJsonArray &rows)
{
    QList<SelectionOption> options;
    for (const auto &row : rows) {
        const auto entry = row.toObject();
        const QString label = entry.value(QStringLiteral("label")).toString().trimmed();
        if (!label.isEmpty())
            options.append({label, entry.value(QStringLiteral("value")).toString().trimmed()});
    }
    return options;
}

QList<SelectionOption> selectionOptions(const QJsonArray &local, const QString &table, const QJsonObject &tables)
{
    if (table.isEmpty())
        return selectionOptions(local);
    QList<SelectionOption> options;
    for (const auto &entry : orderedEnumEntries(tables.value(table).toString()))
        options.append({entry.display.text, entry.code});
    return options;
}

CommandResult selectionFromProperties(const QJsonObject &props, const SelectionOption &option,
                                      const QJsonObject &frameTemplates)
{
    return encode(props, option.value.isEmpty() ? option.label : option.value, nullptr, frameTemplates);
}

namespace {

// HEX mode's command text: the template with {value} in, packed per dataType and byteOrder for a numeric type.
std::optional<QString> hexCommandText(const QJsonObject &props, const QString &templ, const QString &value,
                                      BinaryFrameLayout *layout)
{
    const QString type = prop(props, "dataType", kRawHex);
    QString replacement = value;
    if (type != kRawHex) {
        const auto payload = packedPayload(value, type, bigEndian(props));
        if (!payload)
            return std::nullopt;
        replacement = QString::fromLatin1(payload->toHex());
    }
    const QString text = QString(templ).replace(kPlaceholder, replacement);
    if (layout) {
        QByteArray separate;
        QList<qsizetype> sizes;
        for (const auto &part : templateParts(templ)) {
            const auto bytes = parseHex(part.isInput ? replacement : part.text);
            if (!bytes)
                return text;
            separate += *bytes;
            sizes.append(bytes->size());
        }
        // A literal nibble can share a byte with {value} in a pasted legacy template.
        if (parseHex(text) == separate)
            layout->partSizes = sizes;
    }
    return text;
}

CommandResult bytesCommand(const QJsonObject &props, const QByteArray &bytes)
{
    EncodedCommand command;
    command.route = {props.value(QStringLiteral("commandTargetId")).toString(),
                     props.value(QStringLiteral("commandEndpoint")).toString()};
    command.bytes = bytes;
    return CommandResult::ok(std::move(command));
}

CommandResult textCommand(const QJsonObject &props, const QString &text, const std::optional<QJsonObject> &frame)
{
    if (!frame)
        return bytesCommand(props, (text + newline(prop(props, "commandNewline", QStringLiteral("\\r\\n")))).toUtf8());
    QString error;
    const auto bytes = buildLineFrame(text, *frame, &error);
    if (!bytes)
        return CommandResult::failure(error);
    auto result = bytesCommand(props, *bytes);
    result.command->textEncoding = frame->value(QStringLiteral("encoding")).toString().toLower();
    return result;
}

std::optional<QByteArray> assembleBytes(const QJsonObject &props, const QByteArray &body,
                                       bool can, const std::optional<QJsonObject> &shared, QString *error,
                                       BinaryFrameLayout *layout)
{
    auto *frameLayout = layout ? &layout->frame : nullptr;
    if (shared)
        return FrameTemplates::buildSendFrame(body, *shared, error, frameLayout);
    const auto header = parseHex(can ? QString() : prop(props, "frameHeader", {}));
    const auto footer = parseHex(can ? QString() : prop(props, "frameTail", {}));
    if (!header || !footer) {
        *error = hexError();
        return std::nullopt;
    }
    const auto settings = props.value(QStringLiteral("txFrame"));
    if (!settings.isUndefined() && !settings.isObject()) {
        *error = QCoreApplication::translate("fhmi", "无效的发送帧设置");
        return std::nullopt;
    }
    return buildHexFrame(body, *header, *footer, settings.toObject(), error, frameLayout);
}

CommandResult hexCommand(const QJsonObject &props, const std::optional<QByteArray> &body, BinaryFrameLayout *layout,
                         const std::optional<QJsonObject> &shared)
{
    if (!body)
        return CommandResult::failure(hexError());
    QString error;
    const auto frame = assembleBytes(props, *body, false, shared, &error, layout);
    if (!frame)
        return CommandResult::failure(error);
    auto result = bytesCommand(props, *frame);
    if (shared)
        result.command->framingNotice = FrameTemplates::sendMatchNotice(*frame, *shared);
    return result;
}

QString canHexError(const QString &text)
{
    return QCoreApplication::translate("fhmi", "CAN 数据不是有效的 HEX: %1").arg(text);
}

CommandResult canCommand(const QJsonObject &props, const QString &assignments,
                         std::optional<QByteArray> payload, const QString &payloadError, BinaryFrameLayout *layout,
                         const std::optional<QJsonObject> &shared, bool sendOnce = false)
{
    if (!props.value(QStringLiteral("commandEndpoint")).toString().isEmpty())
        return CommandResult::failure(QCoreApplication::translate("fhmi", "当前模式不支持子通道"));
    EncodedCommand command;
    command.route.connectionId = props.value(QStringLiteral("commandTargetId")).toString();
    command.kind = EncodedCommand::Kind::Can;
    CanCommand &can = command.can;
    can.target = prop(props, "canId", QString()).trimmed();
    if (can.target.isEmpty()) {
        return CommandResult::failure(QCoreApplication::translate("fhmi", "CAN 模式请填写报文名或 CAN ID"));
    }
    const QJsonValue fd = props.value(QStringLiteral("canFd"));
    can.fd = fd.isBool() ? fd.toBool() : fd.toDouble() != 0;
    const int cycle = int(std::clamp<qint64>(integerProp(props, "canCycle", 0).value_or(0), 0, 60000));
    if (sendOnce || cycle > 0)
        can.cycleMs = sendOnce ? 0 : cycle;
    // Whether the message has signals is up to each link; the raw bytes are ready for the links where it has none.
    can.assignments = assignments;
    can.payloadError = payloadError;
    if (payload)
        can.payload = assembleBytes(props, *payload, true, shared, &can.payloadError, layout);
    if (shared && can.payload)
        command.framingNotice = FrameTemplates::sendMatchNotice(*can.payload, *shared);
    if (shared && !FrameTemplates::requiredFields(*shared).isEmpty()) {
        if (!can.payload)
            return CommandResult::failure(can.payloadError);
        can.rawOnly = true;
    }
    return CommandResult::ok(std::move(command));
}

std::optional<QByteArray> packFrameField(const HexField &field, const QString &value)
{
    if (field.type == QLatin1String("ASCII")) {
        if (value.size() > field.length || std::any_of(value.begin(), value.end(), [](QChar ch) {
                return ch.isNull() || ch.unicode() > 0x7f;
            }))
            return std::nullopt;
        return value.toLatin1().leftJustified(field.length, '\0');
    }
    if (field.type == QLatin1String("HEX")) {
        const auto bytes = parseHex(value);
        return bytes && bytes->size() == field.length ? bytes : std::nullopt;
    }
    return packedPayload(value, field.type, !field.littleEndian);
}

enum class FieldEncoding { Send, CheckBindings, DefaultsSample };

CommandResult mappedFrame(const QJsonObject &props, const QJsonValue &bindings, const InputValueReader &read,
                          BinaryFrameLayout *layout, const std::optional<QJsonObject> &shared, bool sendOnce,
                          FieldEncoding encoding = FieldEncoding::Send)
{
    const auto tr = [](const char *s) { return QCoreApplication::translate("fhmi", s); };
    if (!shared)
        return CommandResult::failure(tr("请选择包含字段的 HEX 帧模板"));
    QString error;
    const auto format = hexFrameConfig(*shared, &error);
    if (!format)
        return CommandResult::failure(error);
    const auto overrides = bindings.toArray();
    QSet<QString> overridden;
    for (const auto &row : overrides)
        overridden.insert(row.toObject().value(QStringLiteral("field")).toString());
    const auto defaults = shared->value(FrameTemplates::SendDefaults);
    if (!defaults.isUndefined() && !defaults.isObject())
        return CommandResult::failure(tr("HEX 模板的发送默认值必须是按字段名设置的对象"));
    const auto valuesByName = defaults.toObject();
    const auto required = encoding == FieldEncoding::DefaultsSample ? QStringList{} : FrameTemplates::requiredFields(*shared);
    const auto missing = [&](const QString &name) {
        return CommandResult::failure(tr("必填字段 %1 缺少发送值").arg(name));
    };
    for (const auto &name : required) {
        if (!overridden.contains(name) && valuesByName.value(name).toString().trimmed().isEmpty())
            return missing(name);
    }
    QJsonArray rows;
    for (auto it = valuesByName.constBegin(); it != valuesByName.constEnd(); ++it) {
        if (!overridden.contains(it.key()))
            rows.append(QJsonObject{{QStringLiteral("field"), it.key()}, {QStringLiteral("value"), it.value()}});
    }
    for (const auto &row : overrides)
        rows.append(row);
    if (rows.isEmpty())
        return CommandResult::failure(tr("请为至少一个模板字段设置发送值"));

    const auto frame = format->layout();
    const auto lengthOffset = frame.lengthOffset >= 0 ? frame.lengthOffset - frame.headerSize : 0;
    if (lengthOffset < 0)
        return CommandResult::failure(tr("长度字段位置须在帧头之后、数据末尾以内"));
    const auto maximum = format->maxFrameLength - frame.headerSize - frame.checksumSize - frame.footerSize;
    qsizetype size = lengthOffset + frame.lengthSize;
    if (format->fixedLength > 0)
        size = format->fixedLength - frame.headerSize - frame.checksumSize - frame.footerSize;
    else
        for (const auto &field : format->fields)
            size = std::max(size, qsizetype(field.offset) + field.length);
    if (format->fixedLength == 0)
        size = std::max(size, format->identificationEnd() - frame.headerSize);

    const bool can = prop(props, "commandMode", {}) == QLatin1String("CAN");
    if (size < 0 || size > maximum)
        return CommandResult::failure(tr("模板字段超出可发送的帧长度"));
    QByteArray data(size, '\0');
    QByteArray written(size, '\0');
    const auto fields = FrameTemplates::writableFields(*format);
    QHash<QString, InputValue> samples;
    QList<CommandInputValue> values;
    QList<BinaryFrameLayout::FieldSpan> spans;
    for (const auto &row : rows) {
        const auto binding = row.toObject();
        const auto name = binding.value(QStringLiteral("field")).toString();
        const auto field = std::find_if(fields.begin(), fields.end(), [&](const HexField &item) { return item.name == name; });
        if (field == fields.end())
            return CommandResult::failure(tr("模板字段 %1 不存在或不可写").arg(name));
        if (std::any_of(written.cbegin() + field->offset, written.cbegin() + field->offset + field->length,
                        [](char byte) { return byte != 0; }))
            return CommandResult::failure(tr("模板字段 %1 与另一发送字段的字节范围重叠").arg(name));
        std::fill_n(written.begin() + field->offset, field->length, '\1');

        const auto inputId = binding.value(QStringLiteral("inputId")).toString();
        const auto fixed = binding.value(QStringLiteral("value"));
        const bool needsValue = required.contains(name);
        if (inputId.isEmpty() && !fixed.isString() && !needsValue)
            return CommandResult::failure(tr("请为字段 %1 设置值来源").arg(name));
        if (!inputId.isEmpty() && encoding == FieldEncoding::CheckBindings)
            continue;
        QString value = fixed.toString();
        if (!inputId.isEmpty()) {
            if (!samples.contains(inputId)) {
                const auto input = read ? read(inputId) : std::nullopt;
                if (!input)
                    return needsValue ? missing(name)
                                      : CommandResult::failure(tr("字段 %1 的输入来源 %2 没有有效值").arg(name, inputId));
                samples.insert(inputId, *input);
            }
            value = samples.value(inputId).text();
        }
        if (needsValue && value.trimmed().isEmpty())
            return missing(name);
        const auto bytes = packFrameField(*field, value);
        if (!bytes)
            return CommandResult::failure(tr("字段 %1 无法按 %2 编码，请检查输入值").arg(name, translated(field->type)));
        data.replace(field->offset, field->length, *bytes);
        values.append({name, field->type, value, std::nullopt});
        spans.append({name, frame.headerSize + field->offset, field->length});
    }
    FrameTemplates::fillUnassignedMatchBytes(data, written, *format);
    // The protocol stores field offsets after the frame header, including the computed length slot.
    // Remove that slot from the body so the shared frame builder can insert its length and append the checksum.
    if (frame.lengthSize)
        data.remove(lengthOffset, frame.lengthSize);
    auto result = can ? canCommand(props, {}, std::move(data), {}, layout, shared, sendOnce)
                      : hexCommand(props, std::move(data), layout, shared);
    if (!result.command)
        return result;
    if (can) {
        auto &command = result.command->can;
        if (!command.payload)
            return CommandResult::failure(command.payloadError);
        if (command.payload->size() > (command.fd ? 64 : 8))
            return CommandResult::failure(tr("发送数据超出 CAN 帧的载荷上限"));
        command.rawOnly = true;
    }
    if (layout)
        layout->fields = std::move(spans);
    result.command->inputValues = std::move(values);
    return result;
}

CommandResult templateFromInputs(const QJsonObject &props, InputListMode mode, const QList<CommandInput> &inputs,
                                 const InputValueReader &read, BinaryFrameLayout *layout,
                                 const std::optional<QJsonObject> &shared)
{
    const auto tr = [](const char *source) { return QCoreApplication::translate("fhmi", source); };
    QHash<QString, qsizetype> variables;
    for (qsizetype row = 0; row < inputs.size(); ++row) {
        const QString &name = inputs.at(row).variable;
        if (!validInputVariable(name) || variables.contains(name))
            return CommandResult::failure(tr("输入列表第 %1 行的模板变量无效或重复").arg(row + 1));
        variables.insert(name, row);
    }

    const bool textOnly = mode == InputListMode::Text;
    const bool json = mode == InputListMode::Json;
    const bool can = mode == InputListMode::Can;
    const bool packBytes = mode == InputListMode::Hex || can;
    const bool be = bigEndian(props);
    const QString templ = prop(props, "commandTemplate", kPlaceholder);
    static const QRegularExpression signalSeparators(QStringLiteral("[=,;\\r\\n]"));
    QString text;
    QHash<QString, QString> jsonValues;
    QByteArray bytes;
    QString hexProblem;
    const auto appendLiteral = [&](const QString &literal) {
        text += literal;
        if (packBytes && hexProblem.isEmpty()) {
            if (const auto fragment = parseHex(literal)) {
                bytes += *fragment;
                if (layout)
                    layout->partSizes.append(fragment->size());
            } else {
                hexProblem = can ? canHexError(literal) : hexError();
            }
        }
    };
    QList<std::optional<CommandInputValue>> snapshots(inputs.size());
    bool hasInput = false;
    for (const auto &part : templateParts(templ)) {
        if (!part.isInput) {
            appendLiteral(part.text);
            continue;
        }
        hasInput = true;
        const auto row = variables.constFind(part.text);
        if (row == variables.cend())
            return CommandResult::failure(tr("发送模板中的变量 %1 未在输入列表中定义").arg(inputToken(part.text)));
        const auto &input = inputs.at(*row);
        auto &snapshot = snapshots[*row];
        if (!snapshot) {
            snapshot = readInput(input, read);
            if (!snapshot)
                return missingInput(*row, input);
        }
        // CAN assignments can name several signals in one frame. Input values fill individual fields.
        if (can && snapshot->value.contains(signalSeparators))
            return CommandResult::failure(tr("%1 的输入值包含 CAN 信号分隔符").arg(snapshot->name));

        if (json) {
            if (!jsonValues.contains(part.text))
                jsonValues.insert(part.text, jsonValue(snapshot->value));
            continue;
        }
        text += snapshot->value;
        if (packBytes && hexProblem.isEmpty()) {
            const auto packed = input.dataType == kRawHex ? parseHex(snapshot->value)
                                                         : packedPayload(snapshot->value, input.dataType, be);
            if (packed) {
                bytes += *packed;
                if (layout)
                    layout->partSizes.append(packed->size());
            } else {
                hexProblem = tr("%1 无法按 %2 编码，请检查输入值").arg(snapshot->name, translated(input.dataType));
            }
        }
    }
    if (!hasInput)
        return CommandResult::failure(tr("发送模板至少需要引用一个输入列表变量，如 {p1}"));

    CommandResult result;
    if (textOnly)
        result = textCommand(props, text, shared);
    else if (json)
        result = jsonCommand(props, jsonValues, shared);
    else if (can)
        result = canCommand(props, text, hexProblem.isEmpty() ? std::optional(bytes) : std::nullopt, hexProblem, layout,
                            shared, true);
    else if (hexProblem.isEmpty())
        result = hexCommand(props, bytes, layout, shared);
    else
        return CommandResult::failure(hexProblem);
    if (result.command) {
        for (const auto &snapshot : snapshots)
            if (snapshot)
                result.command->inputValues.append(*snapshot);
    }
    return result;
}

CommandResult frameSequence(const QJsonObject &props, const QJsonObject &library, const InputValueReader &read,
                            FieldEncoding encoding)
{
    const auto entries = props.value(FrameTemplates::SendSequence).toArray();
    QHash<QString, std::optional<InputValue>> values;
    const InputValueReader snapshot = [&values, &read](const QString &id) {
        const auto found = values.constFind(id);
        return found == values.cend() ? *values.insert(id, read ? read(id) : std::nullopt) : *found;
    };
    const auto tr = [](const char *source) { return QCoreApplication::translate("fhmi", source); };
    EncodedCommand batch;
    batch.route = {props.value(QStringLiteral("commandTargetId")).toString(),
                   props.value(QStringLiteral("commandEndpoint")).toString()};
    for (qsizetype index = 0; index < entries.size(); ++index) {
        const auto row = entries.at(index).toObject();
        const auto entry = FrameTemplates::find(library, row.value(FrameTemplates::SendId).toString());
        if (!entry || entry->format.type != QLatin1String("hex"))
            return CommandResult::failure(tr("第 %1 帧缺少有效的 HEX 模板").arg(index + 1));
        const QJsonObject format{{QStringLiteral("commandMode"), QStringLiteral("HEX")}};
        auto result = mappedFrame(format, row.value(FrameTemplates::SendBindings), snapshot, nullptr,
                                  entry->format.settings, true, encoding);
        if (!result.command)
            return CommandResult::failure(tr("第 %1 帧 · %2：%3").arg(index + 1).arg(entry->name, result.error));
        const auto label = QStringLiteral("%1. %2").arg(index + 1).arg(entry->name);
        batch.frames.append({label, result.command->bytes.size()});
        batch.bytes += result.command->bytes;
        if (!result.command->framingNotice.isEmpty()) {
            if (!batch.framingNotice.isEmpty())
                batch.framingNotice += u'\n';
            batch.framingNotice += QStringLiteral("%1 · %2").arg(label, result.command->framingNotice);
        }
        for (auto &value : result.command->inputValues) {
            value.name.prepend(label + QStringLiteral(" · "));
            batch.inputValues.append(std::move(value));
        }
    }
    return CommandResult::ok(std::move(batch));
}

} // namespace

QString frameFieldConfigurationProblem(const QJsonObject &props, const QJsonObject &frameTemplates)
{
    if (usesFrameSequence(props))
        return frameSequence(props, frameTemplates, {}, FieldEncoding::CheckBindings).error;
    return usesFrameFields(props) ? mappedFrame(props, props.value(FrameTemplates::SendBindings), {}, nullptr,
        FrameTemplates::sendSettings(frameTemplates, props), true, FieldEncoding::CheckBindings).error : QString();
}

CommandResult frameFromTemplateDefaults(const QJsonObject &settings)
{
    const QJsonObject properties{{QStringLiteral("commandMode"), QStringLiteral("HEX")}};
    return mappedFrame(properties, QJsonArray{}, {}, nullptr, settings, true, FieldEncoding::DefaultsSample);
}

CommandResult frameFromFieldValues(const QJsonObject &props, const QJsonObject &values,
                                   const QJsonObject &frameTemplates, BinaryFrameLayout *layout)
{
    QJsonArray fields;
    for (auto value = values.cbegin(); value != values.cend(); ++value)
        fields.append(QJsonObject{{QStringLiteral("field"), value.key()}, {QStringLiteral("value"), value.value()}});
    return mappedFrame(props, fields, {}, layout, FrameTemplates::sendSettings(frameTemplates, props), false);
}

CommandResult encode(const QJsonObject &props, const QString &value, BinaryFrameLayout *layout,
                     const QJsonObject &frameTemplates)
{
    if (layout)
        *layout = {};
    const QString selectedMode = prop(props, "commandMode", {});
    const auto mode = commandMode(selectedMode);
    const QString templ = prop(props, "commandTemplate", kPlaceholder);
    const auto shared = FrameTemplates::sendSettings(frameTemplates, props);
    const auto field = props.value(FrameTemplates::SendValueField).toString();
    if ((mode == Mode::Hex || mode == Mode::Can) && !field.isEmpty())
        return mappedFrame(props, QJsonArray{QJsonObject{{QStringLiteral("field"), field}, {QStringLiteral("value"), value}}},
                           {}, layout, shared, false);

    if (mode == Mode::S7)
        return s7FromProperties(props, value);
    if (mode == Mode::Modbus) {
        return modbusFromProperties(props, value);
    }
    if (mode == Mode::Json) {
        return jsonCommand(props, {{QStringLiteral("value"), jsonValue(value)}}, shared);
    }
    if (mode == Mode::Hex || mode == Mode::Can) {
        const auto hexText = hexCommandText(props, templ, value, layout);
        const QString numericError = QCoreApplication::translate("fhmi", "数值无效或超出数据类型范围");
        if (mode == Mode::Can) {
            auto payload = hexText ? parseHex(*hexText) : std::nullopt;
            const QString error = payload ? QString() : hexText ? canHexError(*hexText) : numericError;
            return canCommand(props, QString(templ).replace(kPlaceholder, value), std::move(payload), error, layout, shared);
        }
        return hexText ? hexCommand(props, parseHex(*hexText), layout, shared) : CommandResult::failure(numericError);
    }

    if (mode != Mode::Text)
        return CommandResult::failure(QCoreApplication::translate("fhmi", "未知发送模式：%1").arg(selectedMode));
    return textCommand(props, QString(templ).replace(kPlaceholder, value), shared);
}

CommandResult buttonFromProperties(const QJsonObject &props, const InputValueReader &inputValue,
                                   BinaryFrameLayout *layout, const QJsonObject &frameTemplates)
{
    if (layout)
        *layout = {};
    if (usesFrameSequence(props))
        return frameSequence(props, frameTemplates, inputValue, FieldEncoding::Send);
    if (usesFrameFields(props))
        return mappedFrame(props, props.value(FrameTemplates::SendBindings), inputValue, layout,
                           FrameTemplates::sendSettings(frameTemplates, props), true);
    const auto listMode = inputListMode(props);
    if (listMode == InputListMode::ModbusValue && functionCode(prop(props, "mbFunc", {})) == 0x06 &&
        modbusBit(props) != WholeRegister) {
        // A button's bit write does what mbBitAction says; it reads no input.
        QJsonObject command = props;
        if (!command.contains(QStringLiteral("mbBitAction")))
            command.insert(QStringLiteral("mbBitAction"), bitActions().first());
        return modbusFromProperties(command);
    }
    if (!singleInputProperty(listMode).isEmpty()) {
        const auto inputs = commandInputs(props);
        std::optional<CommandInputValue> input;
        if (!inputs.isEmpty()) {
            input = readInput(inputs.first(), inputValue);
            if (!input)
                return missingInput(0, inputs.first());
        }
        const auto value = input ? std::optional(input->value.trimmed()) : std::nullopt;
        QJsonObject command = props;
        if (listMode == InputListMode::ModbusValue && !command.contains(QStringLiteral("mbFunc")))
            command.insert(QStringLiteral("mbFunc"), functionOptions({0x03}).first());
        auto result = listMode == InputListMode::S7Value ? s7FromProperties(props, value)
                                                        : modbusFromProperties(command, value);
        if (input && result.command)
            result.command->inputValues.append(*input);
        return result;
    }
    if (listMode != InputListMode::None) {
        const auto list = inputListProperty(props);
        if (!list.isUndefined() && !list.isArray())
            return CommandResult::failure(QCoreApplication::translate("fhmi", "输入列表必须是数组"));
        const auto inputs = commandInputs(props);
        if (!inputs.isEmpty()) {
            QHash<QString, InputValue> values;
            const InputValueReader read = [&values, &inputValue](const QString &id) -> std::optional<InputValue> {
                const auto cached = values.constFind(id);
                if (cached != values.cend())
                    return *cached;
                const auto value = inputValue ? inputValue(id) : std::nullopt;
                if (value)
                    values.insert(id, *value);
                return value;
            };
            return listMode == InputListMode::Registers ? modbusWriteFromInputs(props, inputs, read)
                                                        : templateFromInputs(props, listMode, inputs, read, layout,
                                                                             FrameTemplates::sendSettings(frameTemplates, props));
        }
    }

    if (listMode == InputListMode::Registers)
        return modbusFromProperties(props);
    const QString value = prop(props, "command", {}).trimmed();
    return value.isEmpty() ? CommandResult{} : encode(props, value, layout, frameTemplates);
}

} // namespace fhmi::CommandEncoder
