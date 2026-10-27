#include "core/protocol_parser.h"

#include "core/hex_utils.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QtEndian>

#include <cstring>

namespace fhmi {

namespace {

quint16 be16(const QByteArray &buf, int at)
{
    return quint16((quint8(buf.at(at)) << 8) | quint8(buf.at(at + 1)));
}

bool isExceptionCode(int fc)
{
    switch (fc) {
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0x86:
    case 0x8F:
    case 0x90:
        return true;
    default:
        return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// RawFrameBuffer
// ---------------------------------------------------------------------------

RawFrameBuffer::RawFrameBuffer(QByteArray header, QByteArray footer, int maxBuffer)
    : m_header(std::move(header)), m_footer(std::move(footer)), m_maxBuffer(maxBuffer)
{
}

QList<QByteArray> RawFrameBuffer::feed(const QByteArray &data)
{
    if (m_header.isEmpty() || m_footer.isEmpty()) {
        return {data};
    }
    m_buf += data;
    QList<QByteArray> frames;
    while (!m_buf.isEmpty()) {
        const int start = m_buf.indexOf(m_header);
        if (start < 0) {
            const int keep = m_header.size() - 1;
            m_buf = keep ? m_buf.right(keep) : QByteArray();
            break;
        }
        m_buf.remove(0, start);
        int end = m_buf.indexOf(m_footer, m_header.size());
        if (end < 0) {
            if (m_buf.size() > m_maxBuffer) {
                m_buf.remove(0, 1);
                continue;
            }
            break;
        }
        end += m_footer.size();
        if (end <= m_maxBuffer) {
            frames.append(m_buf.left(end));
        }
        m_buf.remove(0, end);
    }
    return frames;
}

// ---------------------------------------------------------------------------
// LineParser
// ---------------------------------------------------------------------------

LineParser::LineParser(QString separator, int maxBuffer) : m_separator(std::move(separator)), m_maxBuffer(maxBuffer) {}

void LineParser::reset()
{
    m_buf.clear();
    m_discarding = false;
}

QList<Message> LineParser::feed(const QByteArray &data)
{
    // Only an actual LF ends a record; JSON may contain an escaped \n.
    QList<Message> results;
    const QList<QByteArray> parts = data.split('\n');
    for (int index = 0; index < parts.size(); ++index) {
        const QByteArray &part = parts.at(index);
        const bool complete = index < parts.size() - 1;
        if (!m_discarding) {
            if (m_buf.size() + part.size() > m_maxBuffer) {
                m_buf.clear();
                m_discarding = true;
            } else {
                m_buf += part;
            }
        }
        if (complete) {
            if (!m_discarding) {
                const QString text = QString::fromUtf8(m_buf).trimmed();
                if (!text.isEmpty()) {
                    Message parsed = parseLine(text);
                    if (!parsed.isEmpty()) {
                        results.append(std::move(parsed));
                    }
                }
            }
            m_buf.clear();
            m_discarding = false;
        }
    }
    return results;
}

Message LineParser::parseLine(const QString &text) const
{
    if (text.startsWith(QLatin1Char('{'))) {
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            return {};
        }
        return doc.object().toVariantMap();
    }

    QStringList parts = text.split(m_separator);
    for (QString &part : parts) {
        part = part.trimmed();
    }

    bool keyValue = false;
    for (const QString &part : parts) {
        if (part.contains(QLatin1Char('='))) {
            keyValue = true;
            break;
        }
    }
    Message result;
    if (keyValue) {
        for (const QString &part : parts) {
            const int eq = part.indexOf(QLatin1Char('='));
            if (eq >= 0) {
                result.insert(part.left(eq).trimmed(), autoNumber(part.mid(eq + 1).trimmed()));
            }
        }
        return result;
    }
    for (int i = 0; i < parts.size(); ++i) {
        result.insert(QStringLiteral("ch%1").arg(i), autoNumber(parts.at(i)));
    }
    return result;
}

QVariant LineParser::autoNumber(const QString &text)
{
    bool ok = false;
    const qlonglong integer = text.toLongLong(&ok);
    if (ok) {
        return integer;
    }
    const double real = text.toDouble(&ok);
    if (ok) {
        return real;
    }
    return text;
}

// ---------------------------------------------------------------------------
// HexFrameParser
// ---------------------------------------------------------------------------

HexFrameParser::HexFrameParser(QByteArray header, QByteArray footer, QList<HexField> fields, int maxBuffer)
    : m_header(std::move(header)), m_footer(std::move(footer)), m_fields(std::move(fields)), m_maxBuffer(maxBuffer)
{
}

QList<Message> HexFrameParser::feed(const QByteArray &data)
{
    m_buf += data;
    QList<Message> results;
    while (!m_buf.isEmpty()) {
        const int start = m_buf.indexOf(m_header);
        if (start < 0) {
            const int keep = m_header.size() - 1;
            m_buf = keep ? m_buf.right(keep) : QByteArray();
            break;
        }
        m_buf.remove(0, start);
        const int end = m_buf.indexOf(m_footer, m_header.size());
        if (end < 0) {
            if (m_buf.size() > m_maxBuffer) {
                m_buf.remove(0, 1);
                continue;
            }
            break;
        }
        const int frameEnd = end + m_footer.size();
        if (frameEnd <= m_maxBuffer) {
            Message parsed = parsePayload(m_buf.mid(m_header.size(), end - m_header.size()));
            if (!parsed.isEmpty()) {
                results.append(std::move(parsed));
            }
        }
        m_buf.remove(0, frameEnd);
    }
    return results;
}

Message HexFrameParser::parsePayload(const QByteArray &payload) const
{
    Message result;
    for (const HexField &field : m_fields) {
        if (field.offset + field.length > payload.size()) {
            continue;
        }
        const QByteArray raw = payload.mid(field.offset, field.length);
        const auto *bytes = reinterpret_cast<const uchar *>(raw.constData());
        if (field.type == QLatin1String("uint8")) {
            result.insert(field.name, qlonglong(bytes[0]));
        } else if (field.type == QLatin1String("int8")) {
            result.insert(field.name, qlonglong(qint8(bytes[0])));
        } else if (field.type == QLatin1String("uint16")) {
            result.insert(field.name, qlonglong(qFromBigEndian<quint16>(bytes)));
        } else if (field.type == QLatin1String("int16")) {
            result.insert(field.name, qlonglong(qFromBigEndian<qint16>(bytes)));
        } else if (field.type == QLatin1String("float32")) {
            const quint32 bits = qFromBigEndian<quint32>(bytes);
            float value = 0;
            std::memcpy(&value, &bits, sizeof value);
            result.insert(field.name, double(value));
        } else {
            result.insert(field.name, QString::fromLatin1(raw.toHex()));
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Modbus
// ---------------------------------------------------------------------------

ModbusRtuParser::ModbusRtuParser(QStringList fields) : m_fields(std::move(fields)) {}

int ModbusRtuParser::frameLength(const QByteArray &buf, int from)
{
    if (buf.size() - from < 3) {
        return -1; // not enough bytes to tell
    }
    const int slave = quint8(buf.at(from));
    if (slave < 1 || slave > 247) {
        return 0;
    }
    const int fc = quint8(buf.at(from + 1));
    if (fc >= 1 && fc <= 4) {
        const int count = quint8(buf.at(from + 2));
        if (count < 1 || count > 250 || ((fc == 3 || fc == 4) && count % 2)) {
            return 0;
        }
        return count + 5;
    }
    if (fc == 5 || fc == 6 || fc == 15 || fc == 16) {
        return 8;
    }
    if (isExceptionCode(fc)) {
        return 5;
    }
    return 0;
}

QList<Message> ModbusRtuParser::feed(const QByteArray &data)
{
    m_buf += data;
    QList<Message> results;
    while (m_buf.size() >= 5) {
        const int length = frameLength(m_buf);
        if (length <= 0) {
            m_buf.remove(0, 1);
            continue;
        }
        if (m_buf.size() < length) {
            // Noise may imitate a long header. Resync only to a complete, CRC-verified
            // response; otherwise keep the partial frame.
            bool found = false;
            for (int offset = 1; offset < m_buf.size() - 4; ++offset) {
                const int size = frameLength(m_buf, offset);
                if (size > 0 && m_buf.size() - offset >= size &&
                    modbusCrc(m_buf.mid(offset, size - 2)) == m_buf.mid(offset + size - 2, 2)) {
                    m_buf.remove(0, offset);
                    found = true;
                    break;
                }
            }
            if (found) {
                continue;
            }
            break;
        }
        const QByteArray frame = m_buf.left(length);
        if (modbusCrc(frame.left(length - 2)) != frame.right(2)) {
            m_buf.remove(0, 1);
            continue;
        }
        m_buf.remove(0, length);
        results.append(modbusResponse(quint8(frame.at(0)), quint8(frame.at(1)), frame.mid(2, length - 4), m_fields));
    }
    return results;
}

ModbusTcpParser::ModbusTcpParser(QStringList fields) : m_fields(std::move(fields)) {}

int ModbusTcpParser::frameLength(const QByteArray &buf)
{
    if (be16(buf, 2) != 0) {
        return 0;
    }
    const int length = be16(buf, 4);
    const int fc = quint8(buf.at(7));
    if (fc >= 1 && fc <= 4) {
        const int count = quint8(buf.at(8));
        if (count < 1 || count > 250 || ((fc == 3 || fc == 4) && count % 2) || length != count + 3) {
            return 0;
        }
    } else if (fc == 5 || fc == 6 || fc == 15 || fc == 16) {
        if (length != 6) {
            return 0;
        }
    } else if (isExceptionCode(fc)) {
        if (length != 3) {
            return 0;
        }
    } else {
        return 0;
    }
    return 6 + length;
}

QList<Message> ModbusTcpParser::feed(const QByteArray &data)
{
    m_buf += data;
    QList<Message> results;
    while (m_buf.size() >= 9) { // the shortest ADU is an exception response
        const int length = frameLength(m_buf);
        if (!length) {
            m_buf.remove(0, 1);
            continue;
        }
        if (m_buf.size() < length) {
            break;
        }
        const QByteArray frame = m_buf.left(length);
        m_buf.remove(0, length);
        results.append(modbusResponse(quint8(frame.at(6)), quint8(frame.at(7)), frame.mid(8), m_fields));
    }
    return results;
}

Message modbusResponse(int slave, int fc, const QByteArray &data, const QStringList &fields)
{
    Message result;
    result.insert(QStringLiteral("slave"), qlonglong(slave));
    result.insert(QStringLiteral("fc"), qlonglong(fc));
    if (fc == 0x03 || fc == 0x04) {
        const QByteArray payload = data.mid(1);
        for (int i = 0, idx = 0; i + 1 < payload.size(); i += 2, ++idx) {
            const QString name = idx < fields.size() ? fields.at(idx) : QStringLiteral("reg%1").arg(idx);
            result.insert(name, qlonglong(be16(payload, i)));
        }
    } else if (fc == 0x01 || fc == 0x02) {
        int idx = 0;
        for (int i = 1; i < data.size(); ++i) {
            const quint8 byte = quint8(data.at(i));
            for (int bit = 0; bit < 8; ++bit, ++idx) {
                const QString name = idx < fields.size() ? fields.at(idx) : QStringLiteral("coil%1").arg(idx);
                result.insert(name, qlonglong((byte >> bit) & 1));
            }
        }
    } else if (fc >= 0x80) {
        result.insert(QStringLiteral("error"), qlonglong(data.isEmpty() ? 0 : quint8(data.at(0))));
    } else if (data.size() >= 4) {
        result.insert(QStringLiteral("address"), qlonglong(be16(data, 0)));
        result.insert(QStringLiteral("value"), qlonglong(be16(data, 2)));
    }
    return result;
}

QByteArray modbusCrc(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (const char c : data) {
        crc ^= quint8(c);
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 1) ? quint16((crc >> 1) ^ 0xA001) : quint16(crc >> 1);
        }
    }
    QByteArray out(2, Qt::Uninitialized);
    out[0] = char(crc & 0xFF);
    out[1] = char(crc >> 8);
    return out;
}

QByteArray buildModbusRtuFrame(int slave, int func, int address, int value, int count)
{
    QByteArray pdu;
    pdu.append(char(slave));
    pdu.append(char(func));
    pdu.append(char((address >> 8) & 0xFF));
    pdu.append(char(address & 0xFF));
    const int word = (func == 0x05 || func == 0x06) ? value : count;
    pdu.append(char((word >> 8) & 0xFF));
    pdu.append(char(word & 0xFF));
    return pdu + modbusCrc(pdu);
}

QByteArray buildModbusTcpFrame(int unit, int func, int address, int value, int count, int transaction)
{
    const QByteArray rtu = buildModbusRtuFrame(unit, func, address, value, count);
    const QByteArray body = QByteArray(1, char(unit)) + rtu.mid(1, rtu.size() - 3);
    QByteArray adu;
    adu.append(char((transaction >> 8) & 0xFF));
    adu.append(char(transaction & 0xFF));
    adu.append('\0');
    adu.append('\0');
    adu.append(char((body.size() >> 8) & 0xFF));
    adu.append(char(body.size() & 0xFF));
    return adu + body;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

namespace {

QStringList fieldNames(const QJsonObject &config)
{
    QStringList out;
    for (const QJsonValue &value : config.value(QStringLiteral("fields")).toArray()) {
        out.append(value.toString());
    }
    return out;
}

void setError(QString *error, const QString &text)
{
    if (error) {
        *error = text;
    }
}

} // namespace

std::unique_ptr<Parser> createParser(const QString &type, const QJsonObject &config, QString *error)
{
    if (type == QLatin1String("line")) {
        const QString separator = config.value(QStringLiteral("separator")).toString(QStringLiteral(","));
        if (separator.isEmpty()) {
            setError(error, QStringLiteral("行分隔符不能为空"));
            return nullptr;
        }
        return std::make_unique<LineParser>(separator);
    }
    if (type == QLatin1String("hex")) {
        const auto header = parseHex(config.value(QStringLiteral("header")).toString(QStringLiteral("AA55")));
        const auto footer = parseHex(config.value(QStringLiteral("footer")).toString(QStringLiteral("55AA")));
        if (!header || !footer || header->isEmpty() || footer->isEmpty()) {
            setError(error, QStringLiteral("HEX 帧头和帧尾不能为空"));
            return nullptr;
        }
        QList<HexField> fields;
        for (const QJsonValue &value : config.value(QStringLiteral("fields")).toArray()) {
            const QJsonArray item = value.toArray();
            HexField field{item.at(0).toString(), item.at(1).toInt(-1), item.at(2).toInt(0), item.at(3).toString()};
            static const QHash<QString, int> sizes = {{QStringLiteral("uint8"), 1},
                                                      {QStringLiteral("int8"), 1},
                                                      {QStringLiteral("uint16"), 2},
                                                      {QStringLiteral("int16"), 2},
                                                      {QStringLiteral("float32"), 4}};
            if (item.size() != 4 || field.offset < 0 || field.length <= 0 ||
                (sizes.contains(field.type) && sizes.value(field.type) != field.length)) {
                setError(error, QStringLiteral("无效 HEX 字段: %1").arg(field.name));
                return nullptr;
            }
            fields.append(field);
        }
        return std::make_unique<HexFrameParser>(*header, *footer, fields);
    }
    if (type == QLatin1String("modbus")) {
        return std::make_unique<ModbusRtuParser>(fieldNames(config));
    }
    if (type == QLatin1String("modbus_tcp")) {
        return std::make_unique<ModbusTcpParser>(fieldNames(config));
    }
    setError(error, QStringLiteral("未知解析模式: %1").arg(type));
    return nullptr;
}

} // namespace fhmi
