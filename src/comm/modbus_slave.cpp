#include "comm/modbus_slave.h"

#include "core/hex_utils.h"
#include "core/i18n.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <set>

namespace fhmi {

namespace {

const QString kInfo = QStringLiteral("info");
const QString kOk = QStringLiteral("ok");
const QString kWarn = QStringLiteral("warn");

quint16 u16(const QByteArray &bytes, int at)
{
    return quint16((quint8(bytes.at(at)) << 8) | quint8(bytes.at(at + 1)));
}

void putU16(QByteArray &bytes, int value)
{
    bytes.append(char((value >> 8) & 0xFF));
    bytes.append(char(value & 0xFF));
}

// The log lines are made of translated pieces; untranslated they read exactly as the vectors shared with PyQt UI
// Designer pin them.
QString functionName(int fc)
{
    static const QHash<int, const char *> names = {{1, QT_TR_NOOP("读线圈")},       {2, QT_TR_NOOP("读离散输入")},
                                                   {3, QT_TR_NOOP("读保持寄存器")}, {4, QT_TR_NOOP("读输入寄存器")},
                                                   {5, QT_TR_NOOP("写单线圈")},     {6, QT_TR_NOOP("写单寄存器")},
                                                   {15, QT_TR_NOOP("写多个线圈")},  {16, QT_TR_NOOP("写多个寄存器")}};
    const char *name = names.value(fc);
    return name ? translated(name) : QString();
}

QString exceptionText(int code)
{
    static const QHash<int, const char *> texts = {
        {1, QT_TR_NOOP("非法功能码")}, {2, QT_TR_NOOP("非法数据地址")}, {3, QT_TR_NOOP("非法数据值")}};
    const char *text = texts.value(code);
    return QCoreApplication::translate("fhmi", "异常 %1 %2")
        .arg(QStringLiteral("%1").arg(code, 2, 16, QLatin1Char('0')), text ? translated(text) : QString());
}

// What a request asks for, as its log line names it.
QString describe(const QByteArray &pdu)
{
    const int fc = quint8(pdu.at(0));
    const QString name = functionName(fc);
    if (name.isEmpty()) {
        return QCoreApplication::translate("fhmi", "功能码 %1")
            .arg(QStringLiteral("%1").arg(fc, 2, 16, QLatin1Char('0')).toUpper());
    }
    if (pdu.size() < 5) {
        return name;
    }
    const int address = u16(pdu, 1);
    const int value = u16(pdu, 3);
    if (fc == 5) {
        return name + QLatin1Char(' ') +
               QCoreApplication::translate("fhmi", "地址 %1 值 %2")
                   .arg(QString::number(address), QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0')).toUpper());
    }
    if (fc == 6) {
        return name + QLatin1Char(' ') + QCoreApplication::translate("fhmi", "地址 %1 值 %2").arg(address).arg(value);
    }
    return name + QLatin1Char(' ') + QCoreApplication::translate("fhmi", "地址 %1 数量 %2").arg(address).arg(value);
}

struct Handled
{
    int exception = 0; // 0: answered with `reply`
    QByteArray reply;  // the reply PDU
    QString table;     // what it wrote, if anything
    QList<int> addresses;
};

Handled exception(int code)
{
    Handled out;
    out.exception = code;
    return out;
}

// One request PDU against the table, as a slave answers it.
Handled handle(ModbusSlaveStore &store, const QByteArray &pdu)
{
    const int fc = quint8(pdu.at(0));
    if (functionName(fc).isEmpty()) {
        return exception(1);
    }
    if (fc <= 6) {
        if (pdu.size() != 5) {
            return exception(3);
        }
    } else if (pdu.size() < 6 || pdu.size() != 6 + quint8(pdu.at(5))) {
        return exception(3);
    }
    const int address = u16(pdu, 1);
    const int quantity = u16(pdu, 3);
    Handled out;
    if (fc == 1 || fc == 2) {
        const QString table = fc == 1 ? QStringLiteral("co") : QStringLiteral("di");
        if (quantity < 1 || quantity > 2000) {
            return exception(3);
        }
        if (!store.contains(table, address, quantity)) {
            return exception(2);
        }
        QByteArray bits((quantity + 7) / 8, '\0');
        for (int i = 0; i < quantity; ++i) {
            if (store.value(table, address + i)) {
                bits[i / 8] = char(quint8(bits.at(i / 8)) | (1 << (i % 8)));
            }
        }
        out.reply = QByteArray(1, char(fc)) + char(bits.size()) + bits;
        return out;
    }
    if (fc == 3 || fc == 4) {
        const QString table = fc == 3 ? QStringLiteral("hr") : QStringLiteral("ir");
        if (quantity < 1 || quantity > 125) {
            return exception(3);
        }
        if (!store.contains(table, address, quantity)) {
            return exception(2);
        }
        QByteArray registers;
        for (int i = 0; i < quantity; ++i) {
            putU16(registers, store.value(table, address + i));
        }
        out.reply = QByteArray(1, char(fc)) + char(registers.size()) + registers;
        return out;
    }
    if (fc == 5) {
        if (quantity != 0x0000 && quantity != 0xFF00) {
            return exception(3);
        }
        if (!store.contains(QStringLiteral("co"), address)) {
            return exception(2);
        }
        store.write(QStringLiteral("co"), address, {quint16(quantity == 0xFF00 ? 1 : 0)});
        out.reply = pdu;
        out.table = QStringLiteral("co");
        out.addresses = {address};
        return out;
    }
    if (fc == 6) {
        if (!store.contains(QStringLiteral("hr"), address)) {
            return exception(2);
        }
        store.write(QStringLiteral("hr"), address, {quint16(quantity)});
        out.reply = pdu;
        out.table = QStringLiteral("hr");
        out.addresses = {address};
        return out;
    }
    const int byteCount = quint8(pdu.at(5));
    QList<quint16> values;
    if (fc == 15) {
        if (quantity < 1 || quantity > 1968 || byteCount != (quantity + 7) / 8) {
            return exception(3);
        }
        if (!store.contains(QStringLiteral("co"), address, quantity)) {
            return exception(2);
        }
        for (int i = 0; i < quantity; ++i) {
            values.append((quint8(pdu.at(6 + i / 8)) >> (i % 8)) & 1);
        }
        out.table = QStringLiteral("co");
    } else {
        if (quantity < 1 || quantity > 123 || byteCount != 2 * quantity) {
            return exception(3);
        }
        if (!store.contains(QStringLiteral("hr"), address, quantity)) {
            return exception(2);
        }
        for (int i = 0; i < quantity; ++i) {
            values.append(u16(pdu, 6 + 2 * i));
        }
        out.table = QStringLiteral("hr");
    }
    store.write(out.table, address, values);
    out.reply = pdu.left(5);
    for (int i = 0; i < quantity; ++i) {
        out.addresses.append(address + i);
    }
    return out;
}

// A run of decimal digits as Python's int() reads it (any Unicode digit); nullopt past `limit`.
std::optional<qlonglong> digits(const QString &text, qlonglong limit)
{
    qlonglong value = 0;
    for (const QChar c : text) {
        value = value * 10 + c.digitValue();
        if (value > limit) {
            return std::nullopt;
        }
    }
    return value;
}

} // namespace

// ---------------------------------------------------------------------------
// ModbusSlaveStore
// ---------------------------------------------------------------------------

const QStringList &ModbusSlaveStore::tables()
{
    static const QStringList names = {QStringLiteral("co"), QStringLiteral("di"), QStringLiteral("hr"),
                                      QStringLiteral("ir")};
    return names;
}

int ModbusSlaveStore::indexOf(const QString &table)
{
    return int(tables().indexOf(table));
}

int ModbusSlaveStore::readFunction(const QString &table)
{
    return indexOf(table) + 1;
}

ModbusSlaveStore::ModbusSlaveStore(const QHash<QString, Area> &areas, const QString &init)
{
    for (int i = 0; i < 4; ++i) {
        const Area wanted = areas.value(tables().at(i));
        Table &table = m_tables[i];
        table.area.start = std::clamp(wanted.start, 0, 65535);
        table.area.count = std::min({std::max(wanted.count, 0), 10000, 65536 - table.area.start});
        table.values = QList<quint16>(table.area.count, 0);
    }
    // Lines as Python's str.splitlines() breaks them, read as both designers read them.
    static const QRegularExpression lineBreak(
        QStringLiteral("\\r\\n|[\\n\\r\\x{0b}\\x{0c}\\x{1c}\\x{1d}\\x{1e}\\x{85}\\x{2028}\\x{2029}]"));
    static const QRegularExpression entry(QStringLiteral("^\\s*(co|di|hr|ir)(\\d+)\\s*=\\s*(-?)(\\d+)\\s*$"),
                                          QRegularExpression::CaseInsensitiveOption |
                                              QRegularExpression::UseUnicodePropertiesOption);
    for (const QString &line : init.split(lineBreak)) {
        const QRegularExpressionMatch match = entry.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        const QString table = match.captured(1).toLower();
        const auto address = digits(match.captured(2), 65535);
        const auto magnitude = digits(match.captured(4), 65535);
        if (!address || !magnitude) {
            continue; // an address no area has, or a value no table takes
        }
        const qlonglong value = match.captured(3).isEmpty() ? *magnitude : -*magnitude;
        if (!contains(table, int(*address))) {
            continue;
        }
        if (table == QLatin1String("co") || table == QLatin1String("di")) {
            if (value != 0 && value != 1) {
                continue;
            }
        } else if (value < -32768) {
            continue;
        }
        write(table, int(*address), {quint16(value & 0xFFFF)});
    }
}

ModbusSlaveStore ModbusSlaveStore::fromProperties(const QJsonObject &props)
{
    static const QHash<QString, QString> names = {{QStringLiteral("co"), QStringLiteral("Co")},
                                                  {QStringLiteral("di"), QStringLiteral("Di")},
                                                  {QStringLiteral("hr"), QStringLiteral("Hr")},
                                                  {QStringLiteral("ir"), QStringLiteral("Ir")}};
    QHash<QString, Area> areas;
    for (const QString &table : tables()) {
        const QString name = QStringLiteral("slave") + names.value(table);
        areas.insert(table, {props.value(name + QStringLiteral("Start")).toInt(0),
                             props.value(name + QStringLiteral("Count")).toInt(100)});
    }
    return ModbusSlaveStore(areas, props.value(QStringLiteral("slaveInit")).toString());
}

ModbusSlaveStore::Area ModbusSlaveStore::area(const QString &table) const
{
    const int index = indexOf(table);
    return index < 0 ? Area{} : m_tables[index].area;
}

bool ModbusSlaveStore::contains(const QString &table, int address, int count) const
{
    const Area a = area(table);
    return count >= 1 && a.start <= address && address + count <= a.start + a.count;
}

quint16 ModbusSlaveStore::value(const QString &table, int address) const
{
    if (!contains(table, address)) {
        return 0;
    }
    const Table &t = m_tables[indexOf(table)];
    return t.values.at(address - t.area.start);
}

void ModbusSlaveStore::write(const QString &table, int address, const QList<quint16> &values)
{
    if (!contains(table, address, int(values.size()))) {
        return;
    }
    Table &t = m_tables[indexOf(table)];
    for (int i = 0; i < values.size(); ++i) {
        t.values[address - t.area.start + i] = values.at(i);
    }
}

Message ModbusSlaveStore::changeMessage(int slave, const QString &table, const QList<int> &addresses) const
{
    std::set<int> keys;
    for (const int address : addresses) {
        for (const int near : {address - 1, address, address + 1}) {
            if (contains(table, near)) {
                keys.insert(near);
            }
        }
    }
    Message message;
    message.insert(QStringLiteral("slave"), qlonglong(slave));
    message.insert(QStringLiteral("fc"), qlonglong(readFunction(table)));
    for (const int address : keys) {
        message.insert(table + QString::number(address), qlonglong(value(table, address)));
    }
    return message;
}

QList<Message> ModbusSlaveStore::startMessages(int slave) const
{
    QList<Message> out;
    for (const QString &table : tables()) {
        const Area a = area(table);
        if (a.count == 0) {
            continue;
        }
        Message message;
        message.insert(QStringLiteral("slave"), qlonglong(slave));
        message.insert(QStringLiteral("fc"), qlonglong(readFunction(table)));
        for (int address = a.start; address < a.start + a.count; ++address) {
            message.insert(table + QString::number(address), qlonglong(value(table, address)));
        }
        out.append(message);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Answering a request
// ---------------------------------------------------------------------------

ModbusSlaveAnswer answerModbusRequest(ModbusSlaveStore &store, ModbusFraming framing, int slave, bool exceptions,
                                      const QByteArray &frame)
{
    ModbusSlaveAnswer answer;
    const bool rtu = framing == ModbusFraming::Rtu;
    if (rtu ? frame.size() < 4 : frame.size() < 8) {
        return answer;
    }
    const int unit = quint8(frame.at(rtu ? 0 : 6));
    const QByteArray pdu = rtu ? frame.mid(1, frame.size() - 3) : frame.mid(7);
    if (rtu ? unit != 0 && unit != slave : unit != 0 && unit != 255 && unit != slave) {
        return answer;
    }
    answer.handled = true;
    const bool broadcast = rtu && unit == 0; // TCP has no broadcast
    const int fc = quint8(pdu.at(0));
    const QString line =
        QStringLiteral("%1 %2 · %3").arg(QCoreApplication::translate("fhmi", "收"), toHexString(frame), describe(pdu));
    const QString unanswered =
        QStringLiteral(" · %1 · %2")
            .arg(QCoreApplication::translate("fhmi", "广播"), QCoreApplication::translate("fhmi", "不应答"));
    if (broadcast && fc >= 1 && fc <= 4) {
        answer.log.append({line + unanswered, kInfo});
        return answer;
    }
    const Handled result = handle(store, pdu);
    if (!result.table.isEmpty()) {
        answer.emits.append(store.changeMessage(slave, result.table, result.addresses));
    }
    QByteArray replyPdu;
    QString tail;
    if (result.exception) {
        answer.failed = true;
        const QString problem = exceptionText(result.exception);
        if (broadcast) {
            answer.log.append({line + QStringLiteral(" · ") + problem + unanswered, kWarn});
            return answer;
        }
        if (!exceptions) {
            answer.log.append(
                {QStringLiteral("%1 · %2 · %3").arg(line, problem, QCoreApplication::translate("fhmi", "不应答")),
                 kWarn});
            return answer;
        }
        replyPdu = QByteArray(1, char(fc | 0x80)) + char(result.exception);
        tail = problem;
    } else {
        if (broadcast) {
            answer.log.append({line + unanswered, kInfo});
            return answer;
        }
        replyPdu = result.reply;
        tail = QCoreApplication::translate("fhmi", "正常");
    }
    if (rtu) {
        const QByteArray body = QByteArray(1, char(unit)) + replyPdu;
        answer.reply = body + modbusCrc(body);
    } else {
        answer.reply = frame.left(4);
        putU16(answer.reply, int(replyPdu.size()) + 1);
        answer.reply += char(unit) + replyPdu;
    }
    answer.log.append({line, kInfo});
    answer.replyLog = {
        QStringLiteral("%1 %2 · %3").arg(QCoreApplication::translate("fhmi", "发"), toHexString(answer.reply), tail),
        result.exception ? kWarn : kOk};
    return answer;
}

// ---------------------------------------------------------------------------
// Framers
// ---------------------------------------------------------------------------

int ModbusRtuRequestFramer::knownLength(int from) const
{
    if (m_buf.size() - from < 2) {
        return -1;
    }
    const int fc = quint8(m_buf.at(from + 1));
    if (fc >= 1 && fc <= 6) {
        return 8;
    }
    if (fc == 15 || fc == 16) {
        return m_buf.size() - from >= 7 ? 9 + quint8(m_buf.at(from + 6)) : -1;
    }
    return 0;
}

bool ModbusRtuRequestFramer::wholeAt(int from) const
{
    const int size = knownLength(from);
    return size > 0 && m_buf.size() - from >= size &&
           modbusCrc(m_buf.mid(from, size - 2)) == m_buf.mid(from + size - 2, 2);
}

QList<QByteArray> ModbusRtuRequestFramer::frames(const QByteArray &data)
{
    m_buf += data;
    QList<QByteArray> out;
    while (m_buf.size() >= 2) {
        const int need = knownLength(0);
        if (need < 0) {
            break;
        }
        if (need == 0) {
            // Another function code: its length is known only at the silence after it, unless a whole request
            // follows it (noise, or a request this cannot frame, before it).
            int later = -1;
            for (int i = 1; i < m_buf.size() - 1; ++i) {
                if (wholeAt(i)) {
                    later = i;
                    break;
                }
            }
            if (later < 0) {
                break;
            }
            m_buf.remove(0, later);
            continue;
        }
        if (m_buf.size() < need) {
            break;
        }
        if (modbusCrc(m_buf.left(need - 2)) == m_buf.mid(need - 2, 2)) {
            out.append(m_buf.left(need));
            m_buf.remove(0, need);
        } else {
            m_buf.remove(0, 1);
        }
    }
    return out;
}

QList<QByteArray> ModbusRtuRequestFramer::flush()
{
    const QByteArray buf = m_buf;
    m_buf.clear();
    if (buf.size() >= 4 && modbusCrc(buf.chopped(2)) == buf.right(2)) {
        return {buf};
    }
    return {};
}

QList<QByteArray> ModbusTcpRequestFramer::frames(const QByteArray &data)
{
    m_buf += data;
    QList<QByteArray> out;
    while (m_buf.size() >= 6) {
        const int protocol = u16(m_buf, 2);
        const int length = u16(m_buf, 4);
        if (protocol != 0 || length < 2 || length > 254) {
            m_buf.clear();
            break;
        }
        if (m_buf.size() < 6 + length) {
            break;
        }
        out.append(m_buf.left(6 + length));
        m_buf.remove(0, 6 + length);
    }
    return out;
}

// ---------------------------------------------------------------------------
// ModbusSlave
// ---------------------------------------------------------------------------

ModbusSlave::Settings ModbusSlave::settingsFromProperties(const QJsonObject &props)
{
    Settings settings;
    settings.slave = std::clamp(props.value(QStringLiteral("modbusSlave")).toInt(1), 1, 247);
    settings.delayMs = std::clamp(props.value(QStringLiteral("modbusDelay")).toInt(0), 0, 5000);
    settings.exceptions = props.value(QStringLiteral("modbusExceptions")).toBool(true);
    return settings;
}

ModbusSlave::ModbusSlave(const Settings &settings, ModbusSlaveStore store, QObject *parent)
    : QObject(parent), m_settings(settings), m_store(std::move(store))
{
}

ModbusSlaveAnswer ModbusSlave::request(ModbusFraming framing, const QByteArray &frame)
{
    ModbusSlaveAnswer answer = answerModbusRequest(m_store, framing, m_settings.slave, m_settings.exceptions, frame);
    if (!answer.handled) {
        return answer;
    }
    for (const auto &[text, level] : std::as_const(answer.log)) {
        emit logged(text, level);
    }
    if (answer.failed) {
        ++m_errors;
        emit statusChanged();
    }
    for (const Message &message : std::as_const(answer.emits)) {
        emit changed(message);
    }
    return answer;
}

void ModbusSlave::replied(const ModbusSlaveAnswer &answer)
{
    ++m_replies;
    emit logged(answer.replyLog.first, answer.replyLog.second);
    emit statusChanged();
}

bool ModbusSlave::write(const QString &table, int address, const QList<quint16> &values)
{
    if (!m_store.contains(table, address, int(values.size()))) {
        return false;
    }
    m_store.write(table, address, values);
    QList<int> addresses;
    for (int i = 0; i < values.size(); ++i) {
        addresses.append(address + i);
    }
    emit changed(m_store.changeMessage(m_settings.slave, table, addresses));
    return true;
}

bool ModbusSlave::writeValue(const QString &table, int address, double value, const QString &dataType,
                             const QString &regOrder)
{
    if (!std::isfinite(value)) {
        return false;
    }
    if (table == QLatin1String("co") || table == QLatin1String("di")) {
        return write(table, address, {quint16(value != 0 ? 1 : 0)});
    }
    QString text;
    if (dataType == QLatin1String("float32")) {
        text = QString::number(std::clamp(value, double(-FLT_MAX), double(FLT_MAX)), 'g', 17);
    } else {
        static const QHash<QString, QPair<double, double>> ranges = {
            {QStringLiteral("uint16"), {0, 65535}},
            {QStringLiteral("int16"), {-32768, 32767}},
            {QStringLiteral("uint32"), {0, 4294967295.0}},
            {QStringLiteral("int32"), {-2147483648.0, 2147483647}}};
        const auto range = ranges.constFind(dataType);
        if (range == ranges.constEnd()) {
            return false;
        }
        text = QString::number(qlonglong(std::llround(std::clamp(value, range->first, range->second))));
    }
    const auto registers = CommandEncoder::modbusRegisters(text, dataType, regOrder);
    return registers && write(table, address, *registers);
}

bool ModbusSlave::execute(const ModbusRequest &request)
{
    switch (request.func) {
    case 0x05:
        return write(QStringLiteral("co"), request.address, {quint16(request.value != 0 ? 1 : 0)});
    case 0x06:
        return write(QStringLiteral("hr"), request.address, {quint16(request.value)});
    case 0x10:
        return write(QStringLiteral("hr"), request.address, request.registers);
    default:
        return false;
    }
}

QString ModbusSlave::status() const
{
    QString text = tr("从站 %1 · 应答 %2").arg(m_settings.slave).arg(m_replies);
    if (m_errors > 0) {
        text += tr(" · 异常 %1").arg(m_errors);
    }
    return text;
}

void ModbusSlave::resetCounts()
{
    m_replies = 0;
    m_errors = 0;
    emit statusChanged();
}

} // namespace fhmi
