#include "core/protocol_parser.h"

#include "core/gbk_table.h"
#include "core/hex_utils.h"

#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>

#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>

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
    case 0x87:
    case 0x88:
    case 0x8F:
    case 0x90:
    case 0x91:
    case 0x96:
    case 0x97:
        return true;
    default:
        return false;
    }
}

// Responses that carry a byte count after the function code, and the reads among them whose
// data are registers, two bytes each.
bool isCountedResponse(int fc)
{
    return (fc >= 0x01 && fc <= 0x04) || fc == 0x11 || fc == 0x17;
}

bool isRegisterRead(int fc)
{
    return fc == 0x03 || fc == 0x04 || fc == 0x17;
}

// The PDU length (function code included) of a response without a byte count; 0 for none.
int fixedPduLength(int fc)
{
    switch (fc) {
    case 0x05:
    case 0x06:
    case 0x08:
    case 0x0F:
    case 0x10:
        return 5;
    case 0x07:
        return 2;
    case 0x16:
        return 7;
    default:
        return isExceptionCode(fc) ? 2 : 0;
    }
}

} // namespace

QList<Message> Parser::feed(const QByteArray &data)
{
    QList<Message> results;
    for (const QByteArray &unit : frames(data)) {
        results += parse(unit);
    }
    return results;
}

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

namespace LineOptions {

const QStringList &kvSeparators()
{
    static const QStringList names = {QStringLiteral("="), QStringLiteral(":")};
    return names;
}

const QStringList &encodings()
{
    static const QStringList names = {QStringLiteral("utf-8"), QStringLiteral("gbk"), QStringLiteral("ascii")};
    return names;
}

} // namespace LineOptions

namespace {

constexpr char16_t kReplacement = 0xFFFD;

// What a GBK byte pair decodes to, 0 when Python's gbk codec has no character for it.
char16_t gbkPair(quint8 lead, quint8 trail)
{
    if (lead < 0x81 || lead > 0xFE || trail < 0x40 || trail > 0xFE || trail == 0x7F) {
        return 0;
    }
    return gbk::kTable[(lead - 0x81) * gbk::kTrailCount + (trail - (trail > 0x7F ? 0x41 : 0x40))];
}

// Python's gbk decoding: an ASCII byte is itself; a byte that does not start a known pair is one
// replacement character, and the byte after it is read again.
QString decodeGbk(const QByteArray &bytes)
{
    QString out;
    out.reserve(bytes.size());
    for (qsizetype i = 0; i < bytes.size();) {
        const quint8 b = quint8(bytes.at(i));
        if (b < 0x80) {
            out.append(QChar(b));
            ++i;
            continue;
        }
        const char16_t c = i + 1 < bytes.size() ? gbkPair(b, quint8(bytes.at(i + 1))) : 0;
        out.append(QChar(c ? c : kReplacement));
        i += c ? 2 : 1;
    }
    return out;
}

// Python's UTF-8 decoding: one replacement character for each maximal subpart of a sequence that
// is not well-formed (an unexpected byte, or a lead byte and the continuation bytes that fit it).
// Qt's own decoder replaces every byte, so "E4 B8 61" would become two of them and "a", not one.
QString decodeUtf8(const QByteArray &bytes)
{
    QString out;
    out.reserve(bytes.size());
    const qsizetype size = bytes.size();
    for (qsizetype i = 0; i < size;) {
        const quint8 b = quint8(bytes.at(i));
        if (b < 0x80) {
            out.append(QChar(b));
            ++i;
            continue;
        }
        int need = 0;
        char32_t code = 0;
        quint8 low = 0x80; // the range of the first continuation byte; the rest take 80..BF
        quint8 high = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            need = 1;
            code = b & 0x1F;
        } else if (b >= 0xE0 && b <= 0xEF) {
            need = 2;
            code = b & 0x0F;
            low = b == 0xE0 ? 0xA0 : 0x80;  // no overlong forms
            high = b == 0xED ? 0x9F : 0xBF; // no surrogates
        } else if (b >= 0xF0 && b <= 0xF4) {
            need = 3;
            code = b & 0x07;
            low = b == 0xF0 ? 0x90 : 0x80;
            high = b == 0xF4 ? 0x8F : 0xBF; // nothing past U+10FFFF
        } else {
            out.append(QChar(kReplacement));
            ++i;
            continue;
        }
        qsizetype at = i + 1;
        bool complete = true;
        for (int k = 0; k < need; ++k, ++at) {
            const quint8 c = at < size ? quint8(bytes.at(at)) : 0;
            if (at >= size || c < (k ? 0x80 : low) || c > (k ? 0xBF : high)) {
                complete = false;
                break;
            }
            code = (code << 6) | (c & 0x3F);
        }
        if (complete) {
            out.append(QString::fromUcs4(&code, 1));
        } else {
            out.append(QChar(kReplacement));
        }
        i = at;
    }
    return out;
}

// Python's str.isspace(): Qt's QChar::isSpace() and the information separators 1C..1F.
bool isPythonSpace(QChar c)
{
    return c.isSpace() || (c.unicode() >= 0x1C && c.unicode() <= 0x1F);
}

// str.strip() as Python does it.
QString stripped(const QString &text)
{
    qsizetype from = 0;
    qsizetype to = text.size();
    while (from < to && isPythonSpace(text.at(from))) {
        ++from;
    }
    while (to > from && isPythonSpace(text.at(to - 1))) {
        --to;
    }
    return text.mid(from, to - from);
}

// The whitespace Python's bytes.strip() takes off the front of a buffer.
bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// A byte JSON can hold outside a string: brackets, separators, quotes, numbers, the letters of
// true / false / null and JSON's four whitespace bytes.
bool isJsonByte(char c)
{
    static constexpr std::string_view kBytes = "{}[],:\"0123456789-+.eEtrufalsn \t\n\r";
    return kBytes.find(c) != std::string_view::npos;
}

// The length of the JSON object or array `buf` starts with, once its brackets close; 0 while
// they are open; -1 at the first byte outside a string that JSON cannot hold, which makes the
// record no JSON at all. Brackets inside strings, and escaped quotes, do not count.
qsizetype jsonLength(const QByteArray &buf)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (qsizetype i = 0; i < buf.size(); ++i) {
        const char c = buf.at(i);
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
        } else if (!isJsonByte(c)) {
            return -1;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if ((c == '}' || c == ']') && --depth == 0) {
            return i + 1;
        }
    }
    return 0;
}

// A JSON object as its keys, an array as keys "0", "1", ...; empty when the text is no JSON.
Message jsonMessage(const QString &text)
{
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        return {};
    }
    if (doc.isObject()) {
        return doc.object().toVariantMap();
    }
    Message out;
    const QJsonArray items = doc.array();
    for (qsizetype i = 0; i < items.size(); ++i) {
        out.insert(QString::number(i), items.at(i).toVariant());
    }
    return out;
}

// Python's str.encode("gbk", errors="replace"): a character GBK cannot spell is one "?".
QByteArray encodeGbk(const QString &text)
{
    static const QHash<char16_t, quint16> pairs = []() {
        QHash<char16_t, quint16> out;
        for (int lead = 0x81; lead <= 0xFE; ++lead) {
            for (int trail = 0x40; trail <= 0xFE; ++trail) {
                if (const char16_t c = gbkPair(quint8(lead), quint8(trail))) {
                    out.insert(c, quint16(lead << 8 | trail));
                }
            }
        }
        return out;
    }();
    QByteArray out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        if (c < 0x80) {
            out.append(char(c));
        } else if (const auto pair = pairs.constFind(c); pair != pairs.cend()) {
            out.append(char(*pair >> 8)).append(char(*pair & 0xFF));
        } else {
            if (QChar::isHighSurrogate(c) && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
                ++i; // one character outside the BMP, one "?"
            }
            out.append('?');
        }
    }
    return out;
}

QByteArray encodeText(const QString &text, const QString &encoding)
{
    return encoding == QLatin1String("gbk") ? encodeGbk(text) : text.toUtf8();
}

} // namespace

QByteArray lineTerminator(const QJsonObject &config)
{
    return encodeText(config.value(QStringLiteral("terminator")).toString(QStringLiteral("\n")),
                      config.value(QStringLiteral("encoding")).toString(QStringLiteral("utf-8")).toLower());
}

LineParser::LineParser(LineFrameConfig config, int maxBuffer) : m_config(std::move(config)), m_maxBuffer(maxBuffer)
{
    if (m_config.terminator.isEmpty()) {
        // createParser() refuses one; an empty terminator would end a record at every byte.
        m_config.terminator = QStringLiteral("\n");
    }
    m_terminator = encodeText(m_config.terminator, m_config.encoding);
}

void LineParser::reset()
{
    m_buf.clear();
    m_discarding = false;
}

QString LineParser::decode(const QByteArray &bytes, const QString &encoding)
{
    if (encoding == QLatin1String("ascii")) {
        QString out(bytes.size(), Qt::Uninitialized);
        for (qsizetype i = 0; i < bytes.size(); ++i) {
            const quint8 b = quint8(bytes.at(i));
            out[i] = b < 0x80 ? QChar(b) : QChar(kReplacement); // not Latin-1: 0x80..FF are no ASCII
        }
        return out;
    }
    return encoding == QLatin1String("gbk") ? decodeGbk(bytes) : decodeUtf8(bytes);
}

QList<Message> LineParser::parse(const QByteArray &data)
{
    m_buf += data;
    QList<Message> results;
    while (true) {
        if (m_discarding) {
            const qsizetype end = m_buf.indexOf(m_terminator);
            if (end < 0) {
                m_buf = m_buf.right(m_terminator.size() - 1); // a terminator may be split over two reads
                break;
            }
            m_buf.remove(0, end + m_terminator.size());
            m_discarding = false;
        }
        qsizetype blank = 0;
        while (blank < m_buf.size() && isAsciiSpace(m_buf.at(blank))) {
            ++blank;
        }
        m_buf.remove(0, blank);
        if (m_buf.isEmpty()) {
            break;
        }
        if (m_buf.startsWith('{') || m_buf.startsWith('[')) {
            // A JSON value ends where its brackets close, a terminator after it is optional.
            const qsizetype length = jsonLength(m_buf);
            if (length < 0) {
                // A byte no JSON holds: drop the record up to the first terminator from its
                // start, so one broken line costs that line and not the records after it.
                m_discarding = true;
                continue;
            }
            if (length > 0) {
                Message parsed = jsonMessage(decode(m_buf.left(length), m_config.encoding));
                m_buf.remove(0, length);
                if (m_buf.startsWith(m_terminator)) {
                    m_buf.remove(0, m_terminator.size());
                }
                if (!parsed.isEmpty()) {
                    results.append(std::move(parsed));
                }
                continue;
            }
        } else if (const qsizetype end = m_buf.indexOf(m_terminator); end >= 0) {
            const QByteArray raw = m_buf.left(end);
            m_buf.remove(0, end + m_terminator.size());
            const QString text = stripped(decode(raw, m_config.encoding));
            if (!text.isEmpty()) {
                Message parsed = parseRecord(text, raw);
                if (!parsed.isEmpty()) {
                    results.append(std::move(parsed));
                }
            }
            continue;
        }
        if (m_buf.size() <= m_maxBuffer) {
            break; // the rest of this message is still to come
        }
        m_discarding = true; // too long to be a message: drop it up to the next terminator
    }
    return results;
}

Message LineParser::parseRecord(QString text, const QByteArray &raw) const
{
    // An NMEA sentence, $ or ! to *HH, is checked and then read like any other record. The XOR
    // runs over the bytes between: neither stripped whitespace nor a multibyte character holds
    // a $, ! or * byte, so the first $ / ! and the last * in `raw` are the sentence's own.
    const qsizetype size = text.size();
    const auto hexDigit = [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')) ||
               (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
    };
    if (size >= 4 && (text.front() == QLatin1Char('$') || text.front() == QLatin1Char('!')) &&
        text.at(size - 3) == QLatin1Char('*') && hexDigit(text.at(size - 2)) && hexDigit(text.at(size - 1))) {
        qsizetype from = 0;
        while (from < raw.size() && raw.at(from) != '$' && raw.at(from) != '!') {
            ++from;
        }
        const qsizetype star = raw.lastIndexOf('*');
        quint8 sum = 0;
        for (qsizetype i = from + 1; i < star; ++i) {
            sum ^= quint8(raw.at(i));
        }
        if (sum != text.right(2).toUInt(nullptr, 16)) {
            return {};
        }
        text = text.mid(1, size - 4);
    }

    QStringList parts = text.split(m_config.separator);
    for (QString &part : parts) {
        part = stripped(part);
    }

    const QString &kv = m_config.kvSeparator;
    bool keyValue = false;
    for (const QString &part : parts) {
        if (part.contains(kv)) {
            keyValue = true;
            break;
        }
    }
    Message result;
    if (keyValue) {
        for (const QString &part : parts) {
            const qsizetype at = part.indexOf(kv);
            if (at >= 0) {
                result.insert(stripped(part.left(at)), autoNumber(stripped(part.mid(at + kv.size()))));
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

int hexChecksumSize(HexChecksum kind)
{
    switch (kind) {
    case HexChecksum::None:
        return 0;
    case HexChecksum::Sum:
    case HexChecksum::Xor:
    case HexChecksum::Crc8:
        return 1;
    case HexChecksum::Crc16Modbus:
    case HexChecksum::Crc16Ccitt:
        return 2;
    }
    return 0;
}

quint16 hexChecksum(HexChecksum kind, const QByteArray &data)
{
    quint16 value = 0;
    switch (kind) {
    case HexChecksum::None:
        break;
    case HexChecksum::Sum:
        for (const char c : data) {
            value = quint8(value + quint8(c));
        }
        break;
    case HexChecksum::Xor:
        for (const char c : data) {
            value ^= quint8(c);
        }
        break;
    case HexChecksum::Crc8: {
        quint8 crc = 0;
        for (const char c : data) {
            crc ^= quint8(c);
            for (int i = 0; i < 8; ++i) {
                crc = (crc & 0x80) ? quint8((crc << 1) ^ 0x07) : quint8(crc << 1);
            }
        }
        value = crc;
        break;
    }
    case HexChecksum::Crc16Modbus: {
        const QByteArray crc = modbusCrc(data); // low byte first, as Modbus RTU sends it
        value = quint16(quint8(crc.at(0)) | (quint8(crc.at(1)) << 8));
        break;
    }
    case HexChecksum::Crc16Ccitt: {
        quint16 crc = 0xFFFF;
        for (const char c : data) {
            crc ^= quint16(quint8(c) << 8);
            for (int i = 0; i < 8; ++i) {
                crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
            }
        }
        value = crc;
        break;
    }
    }
    return value;
}

namespace HexOptions {

const QStringList &checksums()
{
    static const QStringList names = {QStringLiteral("无"),           QStringLiteral("和校验"),
                                      QStringLiteral("异或"),         QStringLiteral("CRC8"),
                                      QStringLiteral("CRC16-Modbus"), QStringLiteral("CRC16-CCITT")};
    return names;
}

const QStringList &byteOrders()
{
    static const QStringList names = {QStringLiteral("大端(BE)"), QStringLiteral("小端(LE)")};
    return names;
}

const QStringList &lengthCovers()
{
    static const QStringList names = {QStringLiteral("数据"), QStringLiteral("长度字段之后"), QStringLiteral("整帧")};
    return names;
}

const QStringList &fieldTypes()
{
    static const QStringList names = {QStringLiteral("uint8"),   QStringLiteral("int8"),    QStringLiteral("uint16"),
                                      QStringLiteral("int16"),   QStringLiteral("uint32"),  QStringLiteral("int32"),
                                      QStringLiteral("float32"), QStringLiteral("float64"), QStringLiteral("ASCII"),
                                      QStringLiteral("HEX")};
    return names;
}

} // namespace HexOptions

namespace {

// Bytes a fixed-size field type takes; 0 for ASCII / HEX, which take any length.
int fieldTypeSize(const QString &type)
{
    static const QHash<QString, int> sizes = {{QStringLiteral("uint8"), 1},   {QStringLiteral("int8"), 1},
                                              {QStringLiteral("uint16"), 2},  {QStringLiteral("int16"), 2},
                                              {QStringLiteral("uint32"), 4},  {QStringLiteral("int32"), 4},
                                              {QStringLiteral("float32"), 4}, {QStringLiteral("float64"), 8}};
    return sizes.value(type, 0);
}

quint64 readUnsigned(const char *bytes, int size, bool littleEndian)
{
    quint64 value = 0;
    for (int i = 0; i < size; ++i) {
        const int at = littleEndian ? size - 1 - i : i;
        value = (value << 8) | quint8(bytes[at]);
    }
    return value;
}

} // namespace

HexFrameParser::HexFrameParser(HexFrameConfig config) : m_config(std::move(config)) {}

int HexFrameParser::frameLength() const
{
    const HexFrameConfig &c = m_config;
    if (c.fixedLength > 0) {
        return m_buf.size() >= c.fixedLength ? c.fixedLength : -1;
    }
    if (c.lengthOffset >= 0) {
        const int fieldEnd = c.lengthOffset + c.lengthSize;
        if (m_buf.size() < fieldEnd) {
            return -1;
        }
        const int value = int(readUnsigned(m_buf.constData() + c.lengthOffset, c.lengthSize, c.lengthLittleEndian));
        const int trailer = hexChecksumSize(c.checksum) + int(c.footer.size());
        int length = value;
        if (c.lengthCovers == HexFrameConfig::LengthCovers::AfterField) {
            length = fieldEnd + value;
        } else if (c.lengthCovers == HexFrameConfig::LengthCovers::Data) {
            length = fieldEnd + value + trailer;
        }
        // A length that cannot hold the frame's own parts is noise that looked like a header.
        if (length < fieldEnd + trailer || length < c.header.size() || length > MaxFrame) {
            return 0;
        }
        return m_buf.size() >= length ? length : -1;
    }
    // The footer is searched for after the header, as before length fields existed.
    const int end = int(m_buf.indexOf(c.footer, c.header.size()));
    return end < 0 ? -1 : end + int(c.footer.size());
}

bool HexFrameParser::valid(const QByteArray &frame) const
{
    const HexFrameConfig &c = m_config;
    const int size = hexChecksumSize(c.checksum);
    const int at = int(frame.size() - c.footer.size()) - size; // where the checksum bytes start
    if (at < c.header.size()) {
        return false;
    }
    if (!c.footer.isEmpty() && !frame.endsWith(c.footer)) {
        return false;
    }
    if (!size) {
        return true;
    }
    if (c.checksumStart > at) {
        return false;
    }
    const quint16 expected = hexChecksum(c.checksum, frame.mid(c.checksumStart, at - c.checksumStart));
    return readUnsigned(frame.constData() + at, size, c.checksumLittleEndian) == expected;
}

QList<QByteArray> HexFrameParser::frames(const QByteArray &data)
{
    const HexFrameConfig &c = m_config;
    const bool ends = c.fixedLength > 0 || c.lengthOffset >= 0 || !c.footer.isEmpty();
    m_buf += data;
    QList<QByteArray> out;
    while (!m_buf.isEmpty()) {
        if (!c.header.isEmpty()) {
            const int start = int(m_buf.indexOf(c.header));
            if (start < 0) {
                const int keep = int(c.header.size()) - 1;
                m_buf = keep ? m_buf.right(keep) : QByteArray();
                break;
            }
            m_buf.remove(0, start);
        }
        // Only a silence ends the frame without an end rule; flush() hands it out.
        const int length = ends ? frameLength() : -1;
        if (length < 0) {
            if (m_buf.size() > MaxFrame) {
                // Too long without a frame: drop from the front, on to the next header. One cut
                // ends where dropping a byte at a time and looking for the header again would.
                m_buf.remove(0, m_buf.size() - MaxFrame);
                continue;
            }
            break;
        }
        if (length == 0) {
            m_buf.remove(0, 1); // resync: past this header's first byte, on to the next header
            continue;
        }
        if (length > MaxFrame) {
            m_buf.remove(0, length); // a footer found too far away ends an overlong frame, dropped whole
            continue;
        }
        const QByteArray frame = m_buf.left(length);
        if (!valid(frame)) {
            m_buf.remove(0, 1);
            continue;
        }
        out.append(frame);
        m_buf.remove(0, length);
    }
    return out;
}

QList<QByteArray> HexFrameParser::flush()
{
    const HexFrameConfig &c = m_config;
    const QByteArray rest = m_buf;
    m_buf.clear();
    // With an end rule, what is left is half a frame. Without one, the silence ends the frame,
    // which then starts with the header: frames() dropped whatever came before it.
    if (c.fixedLength > 0 || c.lengthOffset >= 0 || !c.footer.isEmpty() || rest.isEmpty() ||
        !rest.startsWith(c.header) || !valid(rest)) {
        return {};
    }
    return {rest};
}

QList<Message> HexFrameParser::parse(const QByteArray &frame)
{
    // Fields count from the first byte after the header and stop before the checksum and footer.
    const int from = int(m_config.header.size());
    const int to = int(frame.size() - m_config.footer.size()) - hexChecksumSize(m_config.checksum);
    const QByteArray payload = to > from ? frame.mid(from, to - from) : QByteArray();
    Message result;
    for (const HexField &field : m_config.fields) {
        if (field.offset + field.length > payload.size()) {
            continue;
        }
        const QByteArray raw = payload.mid(field.offset, field.length);
        const int size = fieldTypeSize(field.type);
        const quint64 bits = size ? readUnsigned(raw.constData(), size, field.littleEndian) : 0;
        if (field.type == QLatin1String("uint8") || field.type == QLatin1String("uint16") ||
            field.type == QLatin1String("uint32")) {
            result.insert(field.name, qlonglong(bits));
        } else if (field.type == QLatin1String("int8")) {
            result.insert(field.name, qlonglong(qint8(bits)));
        } else if (field.type == QLatin1String("int16")) {
            result.insert(field.name, qlonglong(qint16(bits)));
        } else if (field.type == QLatin1String("int32")) {
            result.insert(field.name, qlonglong(qint32(bits)));
        } else if (field.type == QLatin1String("float32")) {
            const quint32 word = quint32(bits);
            float value = 0;
            std::memcpy(&value, &word, sizeof value);
            result.insert(field.name, double(value));
        } else if (field.type == QLatin1String("float64")) {
            double value = 0;
            std::memcpy(&value, &bits, sizeof value);
            result.insert(field.name, value);
        } else if (field.type == QLatin1String("ASCII")) {
            // Up to the first NUL; a byte outside ASCII becomes the replacement character.
            QString text;
            for (const char c : raw) {
                if (c == '\0') {
                    break;
                }
                text.append(quint8(c) < 0x80 ? QChar(quint8(c)) : QChar(QChar::ReplacementCharacter));
            }
            result.insert(field.name, text);
        } else {
            result.insert(field.name, QString::fromLatin1(raw.toHex()));
        }
    }
    if (result.isEmpty()) {
        return {};
    }
    return {result};
}

// ---------------------------------------------------------------------------
// Modbus
// ---------------------------------------------------------------------------

void ModbusRequests::note(int slave, int fc, int address, int count, int transaction)
{
    if (transaction < 0) {
        m_latest.insert(slave << 8 | fc, {address, count, ++m_noted});
        return;
    }
    if (!m_transactions.contains(transaction)) {
        m_order.append(transaction);
    }
    m_transactions.insert(transaction, {slave, fc, address, count});
    while (m_order.size() > 256) {
        m_transactions.remove(m_order.takeFirst());
    }
}

void ModbusRequests::noteFrame(const QByteArray &frame, const QString &parserType)
{
    QByteArray body; // slave, function code and request data
    int transaction = -1;
    if (parserType == QLatin1String("modbus") && frame.size() > 2 && modbusCrc(frame.chopped(2)) == frame.right(2)) {
        body = frame.chopped(2);
    } else if (parserType == QLatin1String("modbus_ascii")) {
        body = modbusAsciiBody(frame).value_or(QByteArray());
    } else if (parserType == QLatin1String("modbus_tcp") && frame.size() > 6 && be16(frame, 2) == 0 &&
               be16(frame, 4) == frame.size() - 6) {
        body = frame.mid(6);
        transaction = be16(frame, 0);
    }
    if (body.size() < 6) {
        return;
    }
    const int fc = quint8(body.at(1));
    if (fc >= 1 && fc <= 4 && body.size() == 6) {
        note(quint8(body.at(0)), fc, be16(body, 2), be16(body, 4), transaction);
    } else if (fc == 0x17 && body.size() >= 11 && body.size() == 11 + 2 * be16(body, 8)) {
        // Read/write multiple registers: the reply holds the registers read, so note the read
        // half, which comes first; the write half follows (address, count, byte count, values).
        note(quint8(body.at(0)), fc, be16(body, 2), be16(body, 4), transaction);
    }
}

std::optional<std::pair<int, int>> ModbusRequests::take(int slave, int fc, int transaction,
                                                        QHash<int, int> *answered) const
{
    fc &= 0x7F; // an exception response answers the same request
    if (transaction < 0) {
        const auto found = m_latest.constFind(slave << 8 | fc);
        if (found == m_latest.constEnd()) {
            return std::nullopt;
        }
        if (answered) {
            if (answered->value(slave << 8 | fc, 0) == found->number) {
                return std::nullopt;
            }
            answered->insert(slave << 8 | fc, found->number);
        }
        return std::pair<int, int>{found->address, found->count};
    }
    const auto found = m_transactions.constFind(transaction); // a server's clients all answer one request
    if (found == m_transactions.constEnd() || found->slave != slave || found->fc != fc) {
        return std::nullopt;
    }
    return std::pair<int, int>{found->address, found->count};
}

ModbusRtuParser::ModbusRtuParser(QStringList fields, std::shared_ptr<ModbusRequests> requests)
    : m_fields(std::move(fields)),
      m_requests(requests ? std::move(requests) : std::make_shared<ModbusRequests>())
{
}

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
    if (isCountedResponse(fc)) {
        const int count = quint8(buf.at(from + 2));
        if (count < 1 || count > 250 || (isRegisterRead(fc) && count % 2)) {
            return 0;
        }
        return count + 5;
    }
    const int pdu = fixedPduLength(fc);
    return pdu ? pdu + 3 : 0; // with the slave address and the CRC
}

QList<Message> ModbusRtuParser::parse(const QByteArray &data)
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
        const int slave = quint8(frame.at(0));
        const int fc = quint8(frame.at(1));
        results.append(modbusResponse(slave, fc, frame.mid(2, length - 4), m_fields,
                                      m_requests->take(slave, fc, -1, &m_answered)));
    }
    return results;
}

ModbusTcpParser::ModbusTcpParser(QStringList fields, std::shared_ptr<ModbusRequests> requests)
    : m_fields(std::move(fields)),
      m_requests(requests ? std::move(requests) : std::make_shared<ModbusRequests>())
{
}

int ModbusTcpParser::frameLength(const QByteArray &buf)
{
    if (be16(buf, 2) != 0) {
        return 0;
    }
    const int length = be16(buf, 4);
    const int fc = quint8(buf.at(7));
    if (isCountedResponse(fc)) {
        const int count = quint8(buf.at(8));
        if (count < 1 || count > 250 || (isRegisterRead(fc) && count % 2) || length != count + 3) {
            return 0;
        }
    } else if (const int pdu = fixedPduLength(fc); !pdu || length != pdu + 1) { // the unit id and the PDU
        return 0;
    }
    return 6 + length;
}

QList<Message> ModbusTcpParser::parse(const QByteArray &data)
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
        const int slave = quint8(frame.at(6));
        const int fc = quint8(frame.at(7));
        results.append(modbusResponse(slave, fc, frame.mid(8), m_fields, m_requests->take(slave, fc, be16(frame, 0))));
    }
    return results;
}

ModbusAsciiParser::ModbusAsciiParser(QStringList fields, std::shared_ptr<ModbusRequests> requests)
    : m_fields(std::move(fields)),
      m_requests(requests ? std::move(requests) : std::make_shared<ModbusRequests>())
{
}

QList<Message> ModbusAsciiParser::parse(const QByteArray &data)
{
    // ':' + 2 digits for each of up to 254 bytes (slave, function code, count, 250 data, LRC) + CR LF.
    constexpr qsizetype MaxFrame = 1 + 2 * 254 + 2;
    m_buf += data;
    QList<Message> results;
    while (true) {
        const qsizetype start = m_buf.indexOf(':');
        if (start < 0) {
            m_buf.clear(); // nothing here can start a frame
            break;
        }
        m_buf.remove(0, start);
        const qsizetype end = m_buf.indexOf("\r\n", 1);
        if (end < 0) {
            if (m_buf.size() > MaxFrame) {
                m_buf.remove(0, 1); // no frame is this long: on to the next ':'
                continue;
            }
            break;
        }
        // A colon always starts a new frame: a segment that is not one is dropped up to the next
        // colon, which may be inside it when a frame was cut short.
        const std::optional<QByteArray> body = modbusAsciiBody(m_buf.left(end + 2));
        if (!body || ModbusRtuParser::frameLength(*body + QByteArray(2, '\0')) != body->size() + 2) {
            m_buf.remove(0, 1);
            continue;
        }
        m_buf.remove(0, end + 2);
        const int slave = quint8(body->at(0));
        const int fc = quint8(body->at(1));
        results.append(
            modbusResponse(slave, fc, body->mid(2), m_fields, m_requests->take(slave, fc, -1, &m_answered)));
    }
    return results;
}

Message modbusResponse(int slave, int fc, const QByteArray &data, const QStringList &fields,
                       const std::optional<std::pair<int, int>> &request)
{
    Message result;
    result.insert(QStringLiteral("slave"), qlonglong(slave));
    result.insert(QStringLiteral("fc"), qlonglong(fc));
    QList<qlonglong> values; // by address, when the read this answers is known
    if (isRegisterRead(fc)) {
        const QByteArray payload = data.mid(1);
        QList<qlonglong> registers;
        for (int i = 0, idx = 0; i + 1 < payload.size(); i += 2, ++idx) {
            const QString name = idx < fields.size() ? fields.at(idx) : QStringLiteral("reg%1").arg(idx);
            registers.append(be16(payload, i));
            result.insert(name, registers.last());
        }
        if (request && request->second == registers.size()) {
            values = registers;
        }
    } else if (fc == 0x01 || fc == 0x02) {
        QList<qlonglong> bits;
        for (int i = 1; i < data.size(); ++i) {
            const quint8 byte = quint8(data.at(i));
            for (int bit = 0; bit < 8; ++bit) {
                const int idx = int(bits.size());
                const QString name = idx < fields.size() ? fields.at(idx) : QStringLiteral("coil%1").arg(idx);
                bits.append((byte >> bit) & 1);
                result.insert(name, bits.last());
            }
        }
        if (request && (request->second + 7) / 8 == data.size() - 1) {
            values = bits.mid(0, request->second); // the rest of the last byte is padding
        }
    } else if (fc >= 0x80) {
        result.insert(QStringLiteral("error"), qlonglong(data.isEmpty() ? 0 : quint8(data.at(0))));
        if (request) {
            result.insert(QStringLiteral("start"), qlonglong(request->first));
        }
    } else if (fc == 0x07) { // read exception status
        if (!data.isEmpty()) {
            result.insert(QStringLiteral("status"), qlonglong(quint8(data.at(0))));
        }
    } else if (fc == 0x08) { // diagnostics: the echoed subfunction and its data
        if (data.size() >= 4) {
            result.insert(QStringLiteral("subfunction"), qlonglong(be16(data, 0)));
            result.insert(QStringLiteral("data"), qlonglong(be16(data, 2)));
        }
    } else if (fc == 0x11) { // report slave ID: the ID, the run indicator, then device specific bytes
        const QByteArray payload = data.mid(1);
        if (!payload.isEmpty()) {
            result.insert(QStringLiteral("id"), qlonglong(quint8(payload.at(0))));
            result.insert(QStringLiteral("run"), qlonglong(payload.size() > 1 && quint8(payload.at(1)) == 0xFF));
            result.insert(QStringLiteral("extra"), QString::fromLatin1(payload.mid(2).toHex()));
        }
    } else if (fc == 0x16) { // mask write register, echoed
        if (data.size() >= 6) {
            result.insert(QStringLiteral("address"), qlonglong(be16(data, 0)));
            result.insert(QStringLiteral("andMask"), qlonglong(be16(data, 2)));
            result.insert(QStringLiteral("orMask"), qlonglong(be16(data, 4)));
        }
    } else if (data.size() >= 4) {
        result.insert(QStringLiteral("address"), qlonglong(be16(data, 0)));
        result.insert(QStringLiteral("value"), qlonglong(be16(data, 2)));
    }
    if (!values.isEmpty()) {
        static const QHash<int, QString> tables = {{0x01, QStringLiteral("co")},
                                                   {0x02, QStringLiteral("di")},
                                                   {0x03, QStringLiteral("hr")},
                                                   {0x04, QStringLiteral("ir")},
                                                   {0x17, QStringLiteral("hr")}};
        const int start = request->first;
        result.insert(QStringLiteral("start"), qlonglong(start));
        for (int offset = 0; offset < values.size(); ++offset) {
            result.insert(tables.value(fc) + QString::number(start + offset), values.at(offset));
        }
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

quint8 modbusLrc(const QByteArray &data)
{
    quint8 sum = 0;
    for (const char c : data) {
        sum = quint8(sum + quint8(c));
    }
    return quint8(-sum);
}

QByteArray modbusAsciiFrame(const QByteArray &body)
{
    return ':' + (body + char(modbusLrc(body))).toHex().toUpper() + "\r\n";
}

std::optional<QByteArray> modbusAsciiBody(const QByteArray &frame)
{
    if (frame.size() < 7 || !frame.startsWith(':') || !frame.endsWith("\r\n") || frame.size() % 2 == 0) {
        return std::nullopt; // ':' + an even number of digits + CR LF is odd
    }
    const QByteArray digits = frame.mid(1, frame.size() - 3);
    for (const char c : digits) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return std::nullopt; // fromHex() would skip it
        }
    }
    const QByteArray bytes = QByteArray::fromHex(digits);
    const QByteArray body = bytes.chopped(1);
    if (modbusLrc(body) != quint8(bytes.back())) {
        return std::nullopt;
    }
    return body;
}

QByteArray buildModbusRtuFrame(int slave, int func, int address, int value, int count,
                               const QList<quint16> &registers)
{
    QByteArray pdu;
    pdu.append(char(slave));
    pdu.append(char(func));
    pdu.append(char((address >> 8) & 0xFF));
    pdu.append(char(address & 0xFF));
    const auto word = [&pdu](int w) {
        pdu.append(char((w >> 8) & 0xFF));
        pdu.append(char(w & 0xFF));
    };
    if (func == 0x10) {
        word(int(registers.size()));
        pdu.append(char(2 * registers.size()));
        for (const quint16 r : registers) {
            word(r);
        }
    } else {
        word((func == 0x05 || func == 0x06) ? value : count);
    }
    return pdu + modbusCrc(pdu);
}

QByteArray buildModbusTcpFrame(int unit, int func, int address, int value, int count, int transaction,
                               const QList<quint16> &registers)
{
    const QByteArray rtu = buildModbusRtuFrame(unit, func, address, value, count, registers);
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

QByteArray buildModbusAsciiFrame(int slave, int func, int address, int value, int count,
                                 const QList<quint16> &registers)
{
    return modbusAsciiFrame(buildModbusRtuFrame(slave, func, address, value, count, registers).chopped(2));
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

// A whole number setting; the fallback when the key is missing, nullopt for anything else.
std::optional<int> intSetting(const QJsonObject &config, const char *key, int fallback)
{
    const QJsonValue value = config.value(QLatin1String(key));
    if (value.isUndefined()) {
        return fallback;
    }
    const double number = value.toDouble(0.5);
    if (!value.isDouble() || number != std::floor(number) || std::fabs(number) > 1e9) {
        return std::nullopt;
    }
    return int(number);
}

// Hex text for a frame delimiter. Unlike command hex, an odd digit count is an error here:
// a delimiter padded with a guessed zero would never match.
std::optional<QByteArray> delimiterSetting(const QJsonObject &config, const char *key, const char *fallback)
{
    const QJsonValue value = config.value(QLatin1String(key));
    if (!value.isUndefined() && !value.isString()) {
        return std::nullopt;
    }
    QString digits = value.toString(QLatin1String(fallback));
    digits.remove(QRegularExpression(QStringLiteral("\\s")));
    if (digits.size() % 2) {
        return std::nullopt;
    }
    return parseHex(digits);
}

std::optional<LineFrameConfig> lineFrameConfig(const QJsonObject &config, QString *error)
{
    const auto fail = [error](const QString &text) {
        setError(error, text);
        return std::nullopt;
    };
    LineFrameConfig out;
    out.separator = config.value(QStringLiteral("separator")).toString(out.separator);
    if (out.separator.isEmpty()) {
        return fail(QStringLiteral("行分隔符不能为空"));
    }
    const QJsonValue terminator = config.value(QStringLiteral("terminator"));
    if (!terminator.isUndefined() && (!terminator.isString() || terminator.toString().isEmpty())) {
        return fail(QStringLiteral("行结束符不能为空"));
    }
    out.terminator = terminator.toString(out.terminator);
    const QJsonValue kv = config.value(QStringLiteral("kvSeparator"));
    if (!kv.isUndefined() && !LineOptions::kvSeparators().contains(kv.toString())) {
        return fail(QStringLiteral("键值分隔符只能是 = 或 :"));
    }
    out.kvSeparator = kv.toString(out.kvSeparator);
    const QJsonValue encoding = config.value(QStringLiteral("encoding"));
    if (!encoding.isUndefined() && !LineOptions::encodings().contains(encoding.toString().toLower())) {
        return fail(QStringLiteral("编码只能是 utf-8、gbk 或 ascii"));
    }
    out.encoding = encoding.toString(out.encoding).toLower();
    return out;
}

std::optional<HexFrameConfig> hexFrameConfig(const QJsonObject &config, QString *error)
{
    const auto fail = [error](const QString &text) {
        setError(error, text);
        return std::nullopt;
    };
    HexFrameConfig out;
    const auto header = delimiterSetting(config, "header", "AA55");
    const auto footer = delimiterSetting(config, "footer", "55AA");
    const auto lengthOffset = intSetting(config, "lengthOffset", -1);
    const auto lengthSize = intSetting(config, "lengthSize", 1);
    const auto fixedLength = intSetting(config, "fixedLength", 0);
    const auto checksumStart = intSetting(config, "checksumStart", 0);
    const auto idleTimeout = intSetting(config, "idleTimeout", 0);

    // Something has to end a frame; a setting that is present but invalid is reported below.
    if (footer && footer->isEmpty() && lengthOffset && *lengthOffset < 0 && fixedLength && *fixedLength == 0 &&
        idleTimeout && *idleTimeout == 0) {
        return fail(QStringLiteral("HEX 帧需要帧尾、长度字段、固定帧长或空闲超时之一来分帧"));
    }
    if (!header || !footer) {
        return fail(QStringLiteral("HEX 帧头或帧尾不是十六进制"));
    }
    out.header = *header;
    out.footer = *footer;
    if (fixedLength && *fixedLength > 0 && lengthOffset && *lengthOffset >= 0) {
        return fail(QStringLiteral("固定帧长和长度字段只能选一个"));
    }
    const QString lengthEndian =
        config.value(QStringLiteral("lengthEndian")).toString(HexOptions::byteOrders().first());
    const QString covers = config.value(QStringLiteral("lengthCovers")).toString(HexOptions::lengthCovers().first());
    const auto stringIn = [&config](const char *key, const QStringList &options) {
        const QJsonValue value = config.value(QLatin1String(key));
        return value.isUndefined() || (value.isString() && options.contains(value.toString()));
    };
    if (!lengthOffset || *lengthOffset < -1 || !lengthSize || (*lengthSize != 1 && *lengthSize != 2) ||
        !stringIn("lengthEndian", HexOptions::byteOrders()) || !stringIn("lengthCovers", HexOptions::lengthCovers())) {
        return fail(QStringLiteral("HEX 长度字段设置无效"));
    }
    out.lengthOffset = *lengthOffset;
    out.lengthSize = *lengthSize;
    out.lengthLittleEndian = lengthEndian == HexOptions::byteOrders().at(1);
    out.lengthCovers = HexFrameConfig::LengthCovers(HexOptions::lengthCovers().indexOf(covers));
    if (!fixedLength || *fixedLength < 0 || *fixedLength > 65536) {
        return fail(QStringLiteral("HEX 固定帧长无效"));
    }
    out.fixedLength = *fixedLength;
    if (!stringIn("checksum", HexOptions::checksums()) || !checksumStart || *checksumStart < 0 ||
        !stringIn("checksumEndian", HexOptions::byteOrders())) {
        return fail(QStringLiteral("HEX 校验设置无效"));
    }
    out.checksum = HexChecksum(HexOptions::checksums().indexOf(
        config.value(QStringLiteral("checksum")).toString(HexOptions::checksums().first())));
    out.checksumStart = *checksumStart;
    out.checksumLittleEndian =
        config.value(QStringLiteral("checksumEndian")).toString() == HexOptions::byteOrders().at(1);
    if (!idleTimeout || *idleTimeout < 0 || *idleTimeout > 60000) {
        return fail(QStringLiteral("HEX 空闲超时无效"));
    }
    out.idleTimeout = *idleTimeout;

    const QJsonValue fieldList = config.value(QStringLiteral("fields"));
    if (!fieldList.isUndefined() && !fieldList.isNull() && !fieldList.isArray()) {
        return fail(QStringLiteral("无效 HEX 字段: ")); // a table that is not a list, as the Python designer reports it
    }
    for (const QJsonValue &value : fieldList.toArray()) {
        const QJsonArray item = value.toArray();
        const QString name = item.at(0).toVariant().toString();
        const auto number = [&item](int at) -> std::optional<int> {
            const QJsonValue v = item.at(at);
            const double d = v.toDouble(0.5);
            if (!v.isDouble() || d != std::floor(d) || std::fabs(d) > 1e9) {
                return std::nullopt;
            }
            return int(d);
        };
        const auto offset = number(1);
        const auto length = number(2);
        const QString type = item.at(3).toString();
        const int size = fieldTypeSize(type);
        const bool endianOk =
            item.size() == 4 || (item.at(4).isString() && HexOptions::byteOrders().contains(item.at(4).toString()));
        if ((item.size() != 4 && item.size() != 5) || !offset || *offset < 0 || !length || *length <= 0 ||
            !HexOptions::fieldTypes().contains(type) || (size && size != *length) || !endianOk) {
            return fail(QStringLiteral("无效 HEX 字段: %1").arg(name));
        }
        out.fields.append(HexField{name, *offset, *length, type,
                                   item.size() == 5 && item.at(4).toString() == HexOptions::byteOrders().at(1)});
    }
    return out;
}

} // namespace

std::unique_ptr<Parser> createParser(const QString &type, const QJsonObject &config, QString *error,
                                     std::shared_ptr<ModbusRequests> requests)
{
    if (type == QLatin1String("line")) {
        auto settings = lineFrameConfig(config, error);
        if (!settings) {
            return nullptr;
        }
        return std::make_unique<LineParser>(std::move(*settings));
    }
    if (type == QLatin1String("hex")) {
        auto settings = hexFrameConfig(config, error);
        if (!settings) {
            return nullptr;
        }
        return std::make_unique<HexFrameParser>(std::move(*settings));
    }
    if (type == QLatin1String("modbus")) {
        return std::make_unique<ModbusRtuParser>(fieldNames(config), std::move(requests));
    }
    if (type == QLatin1String("modbus_tcp")) {
        return std::make_unique<ModbusTcpParser>(fieldNames(config), std::move(requests));
    }
    if (type == QLatin1String("modbus_ascii")) {
        return std::make_unique<ModbusAsciiParser>(fieldNames(config), std::move(requests));
    }
    setError(error, QStringLiteral("未知解析模式: %1").arg(type));
    return nullptr;
}

bool isModbusParser(const QString &type)
{
    return type == QLatin1String("modbus") || type == QLatin1String("modbus_tcp") ||
           type == QLatin1String("modbus_ascii");
}

} // namespace fhmi
