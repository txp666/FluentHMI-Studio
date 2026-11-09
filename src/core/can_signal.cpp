#include "core/can_signal.h"

#include "core/binding.h"
#include "core/dbc.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QtNumeric>

#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>

namespace fhmi {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

quint64 mask(int length)
{
    return length >= 64 ? ~quint64(0) : (quint64(1) << length) - 1;
}

// The bit positions of a signal, MSB first for Motorola and LSB first for Intel, as the DBC numbers them:
// byte = position / 8, bit = position % 8 (bit 0 the least significant of the byte).
template <typename Visit> void forEachBit(const CanSignalDef &signal, Visit visit)
{
    if (!signal.motorola) {
        for (int i = 0; i < signal.length; ++i) {
            visit(signal.start + i, i);
        }
        return;
    }
    // Motorola "sawtooth": from the MSB down through its byte, then on at bit 7 of the next byte.
    int position = signal.start;
    for (int i = 0; i < signal.length; ++i) {
        visit(position, signal.length - 1 - i);
        position = position % 8 == 0 ? position + 15 : position - 1;
    }
}

// The last (highest) bit position a signal covers.
int lastPosition(const CanSignalDef &signal)
{
    int last = 0;
    forEachBit(signal, [&last](int position, int) { last = std::max(last, position); });
    return last;
}

bool integral(double value)
{
    return std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 9.0e18;
}

// The raw value as a signed integer when the signal is signed (two's complement), else as it is.
qint64 signedRaw(const CanSignalDef &signal, quint64 bits)
{
    if (signal.isSigned && signal.length < 64 && ((bits >> (signal.length - 1)) & 1)) {
        bits |= ~mask(signal.length);
    }
    return qint64(bits);
}

QString outOfRange(const CanSignalDef &signal, const QString &value)
{
    return QCoreApplication::translate("fhmi", "信号 %1 的值 %2 超出范围").arg(signal.name, value);
}

// Raw bits for a whole raw value, or nullopt when it is outside the signal's bit range.
std::optional<quint64> integerBits(const CanSignalDef &signal, qint64 raw, bool rawIsUnsigned)
{
    if (signal.isSigned) {
        if (rawIsUnsigned && quint64(raw) > quint64(std::numeric_limits<qint64>::max())) {
            return std::nullopt;
        }
        if (signal.length < 64) {
            const qint64 high = (qint64(1) << (signal.length - 1)) - 1;
            if (raw < -high - 1 || raw > high) {
                return std::nullopt;
            }
        }
        return quint64(raw) & mask(signal.length);
    }
    if (!rawIsUnsigned && raw < 0) {
        return std::nullopt;
    }
    if (quint64(raw) > mask(signal.length)) {
        return std::nullopt;
    }
    return quint64(raw);
}

// The body of an SG_ line, shared by DBC files and canSignals:
// <Name> [M|m<n>] : <start>|<length>@<0|1><+|-> [(<factor>,<offset>)] [[<min>|<max>]] ["<unit>"] [receivers]
const QRegularExpression &signalPattern()
{
    static const QRegularExpression pattern(QStringLiteral(
        "^([A-Za-z_][A-Za-z0-9_]*)\\s*(M|m\\d+M?)?\\s*:\\s*(\\d+)\\s*\\|\\s*(\\d+)\\s*@\\s*([01])\\s*([+-])"
        "\\s*(?:\\(\\s*([^,()]*?)\\s*,\\s*([^,()]*?)\\s*\\))?"
        "\\s*(?:\\[\\s*([^|\\]]*?)\\s*\\|\\s*([^\\]]*?)\\s*\\])?"
        "\\s*(?:\"((?:[^\"\\\\]|\\\\.)*)\")?\\s*(.*)$"));
    return pattern;
}

} // namespace

// ---------------------------------------------------------------------------
// CanSignalDef
// ---------------------------------------------------------------------------

bool CanSignalDef::fits(int dataLength) const
{
    return start >= 0 && length >= 1 && lastPosition(*this) / 8 < dataLength;
}

int CanSignalDef::bytesNeeded() const
{
    return lastPosition(*this) / 8 + 1;
}

quint64 CanSignalDef::rawBits(const QByteArray &data) const
{
    quint64 bits = 0;
    const auto *bytes = reinterpret_cast<const quint8 *>(data.constData());
    forEachBit(*this, [&](int position, int index) {
        if ((bytes[position / 8] >> (position % 8)) & 1) {
            bits |= quint64(1) << index;
        }
    });
    return bits;
}

void CanSignalDef::putRawBits(QByteArray &data, quint64 bits) const
{
    char *bytes = data.data();
    forEachBit(*this, [&](int position, int index) {
        const char bit = char(1 << (position % 8));
        if ((bits >> index) & 1) {
            bytes[position / 8] = char(bytes[position / 8] | bit);
        } else {
            bytes[position / 8] = char(bytes[position / 8] & ~bit);
        }
    });
}

QVariant CanSignalDef::decode(const QByteArray &data) const
{
    if (!fits(int(data.size()))) {
        return QVariant();
    }
    const quint64 bits = rawBits(data);
    if (valueType == ValueType::Float32) {
        const quint32 word = quint32(bits);
        float value = 0;
        std::memcpy(&value, &word, sizeof value);
        return double(value) * factor + offset;
    }
    if (valueType == ValueType::Double) {
        double value = 0;
        std::memcpy(&value, &bits, sizeof value);
        return value * factor + offset;
    }
    const bool hugeUnsigned = !isSigned && bits > quint64(std::numeric_limits<qint64>::max());
    if (hugeUnsigned) {
        return double(bits) * factor + offset;
    }
    const qint64 raw = signedRaw(*this, bits);
    if (integral(factor) && integral(offset)) {
        qint64 scaled = 0;
        qint64 result = 0;
        if (!qMulOverflow(raw, qint64(factor), &scaled) && !qAddOverflow(scaled, qint64(offset), &result)) {
            return qlonglong(result);
        }
    }
    return double(raw) * factor + offset;
}

QString CanSignalDef::valueText(const QByteArray &data) const
{
    if (valueDescriptions.isEmpty() || valueType != ValueType::Integer || !fits(int(data.size()))) {
        return QString();
    }
    return valueDescriptions.value(signedRaw(*this, rawBits(data)));
}

std::optional<quint64> CanSignalDef::encode(const QString &value, QString *error) const
{
    const QString text = value.trimmed();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        // 0x…: the raw bits, taken as they are. Factor, offset and the physical limits are for decimal values; a
        // hex value is what a tester reads off the bus, so it must not be scaled behind their back.
        bool ok = false;
        const quint64 bits = text.mid(2).toULongLong(&ok, 16);
        // toULongLong accepts a sign and spaces, which a bit pattern has no use for.
        const bool digitsOnly = ok && text.size() > 2 && text.at(2).isLetterOrNumber();
        if (!digitsOnly) {
            fail(error, QCoreApplication::translate("fhmi", "信号 %1 的值无效: %2").arg(name, text));
            return std::nullopt;
        }
        if (bits > mask(length)) {
            fail(error, outOfRange(*this, text));
            return std::nullopt;
        }
        return bits;
    }
    if (valueType == ValueType::Integer && factor == 1 && integral(offset)) {
        // Whole numbers are worked exactly, so 64-bit raw values do not go through a double.
        bool ok = false;
        const qlonglong number = text.toLongLong(&ok);
        if (ok) {
            qint64 raw = 0;
            if (qSubOverflow(qint64(number), qint64(offset), &raw)) {
                fail(error, outOfRange(*this, text));
                return std::nullopt;
            }
            const auto bits = integerBits(*this, raw, false);
            if (!bits) {
                fail(error, outOfRange(*this, text));
            }
            return bits;
        }
        const qulonglong big = text.toULongLong(&ok);
        if (ok && offset == 0) {
            const auto bits = integerBits(*this, qint64(big), true);
            if (!bits) {
                fail(error, outOfRange(*this, text));
            }
            return bits;
        }
    }
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (ok) {
        return encode(number, error);
    }
    if (valueType == ValueType::Integer) {
        for (auto it = valueDescriptions.constBegin(); it != valueDescriptions.constEnd(); ++it) {
            if (it.value() == text) {
                const auto bits = integerBits(*this, it.key(), false);
                if (!bits) {
                    fail(error, outOfRange(*this, text));
                }
                return bits;
            }
        }
    }
    fail(error, QCoreApplication::translate("fhmi", "信号 %1 的值无效: %2").arg(name, text));
    return std::nullopt;
}

std::optional<quint64> CanSignalDef::encode(double physical, QString *error) const
{
    const QString shown = QString::number(physical, 'g', 15);
    if (!std::isfinite(physical) || factor == 0) {
        fail(error, QCoreApplication::translate("fhmi", "信号 %1 的值无效: %2").arg(name, shown));
        return std::nullopt;
    }
    const double scaled = (physical - offset) / factor;
    if (valueType == ValueType::Float32) {
        if (!std::isfinite(scaled) || std::fabs(scaled) > double(FLT_MAX)) {
            fail(error, outOfRange(*this, shown));
            return std::nullopt;
        }
        const float value = float(scaled);
        quint32 word = 0;
        std::memcpy(&word, &value, sizeof word);
        return quint64(word);
    }
    if (valueType == ValueType::Double) {
        if (!std::isfinite(scaled)) {
            fail(error, outOfRange(*this, shown));
            return std::nullopt;
        }
        quint64 bits = 0;
        std::memcpy(&bits, &scaled, sizeof bits);
        return bits;
    }
    const double raw = std::round(scaled); // half away from zero
    const double span = std::ldexp(1.0, length); // 2^length
    if (isSigned) {
        if (!(raw >= -span / 2 && raw < span / 2)) {
            fail(error, outOfRange(*this, shown));
            return std::nullopt;
        }
        return quint64(qint64(raw)) & mask(length);
    }
    if (!(raw >= 0 && raw < span)) {
        fail(error, outOfRange(*this, shown));
        return std::nullopt;
    }
    return quint64(raw);
}

quint64 CanSignalDef::initialBits() const
{
    if (!startValue || valueType != ValueType::Integer || !std::isfinite(*startValue)) {
        return 0;
    }
    const double raw = std::round(*startValue);
    if (std::fabs(raw) >= 9.0e18) {
        return 0;
    }
    const auto bits = integerBits(*this, qint64(raw), false);
    return bits ? *bits : 0;
}

bool CanSignalDef::validate(QString *error) const
{
    if (length < 1 || length > 64) {
        return fail(error, QCoreApplication::translate("fhmi", "信号 %1 的长度必须在 1–64 位之间").arg(name));
    }
    if (valueType == ValueType::Float32 && length != 32) {
        return fail(error, QCoreApplication::translate("fhmi", "信号 %1 是 float，长度必须是 32 位").arg(name));
    }
    if (valueType == ValueType::Double && length != 64) {
        return fail(error, QCoreApplication::translate("fhmi", "信号 %1 是 double，长度必须是 64 位").arg(name));
    }
    if (start < 0 || start > 511 || lastPosition(*this) > 511) {
        return fail(error, QCoreApplication::translate("fhmi", "信号 %1 的位置超出 64 字节").arg(name));
    }
    return true;
}

std::optional<int> motorolaMsbStart(int lsbStart, int length)
{
    if (lsbStart < 0 || length < 1 || length > 64) {
        return std::nullopt;
    }
    int position = lsbStart;
    for (int i = 1; i < length; ++i) {
        position = position % 8 == 7 ? position - 15 : position + 1;
        if (position < 0) {
            // Checked at every step: C++'s % of a negative position would walk it back into byte 0.
            return std::nullopt;
        }
    }
    return position;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

// Shared with the DBC parser: fills `signal` from an SG_ body; `mux` gets the multiplexer marker and `rest`
// what follows the unit (the receivers).
bool parseSignalBody(const QString &body, CanSignalDef *signal, QString *mux, QString *rest, QString *error)
{
    const QRegularExpressionMatch match = signalPattern().match(body.trimmed());
    if (!match.hasMatch()) {
        return fail(error, QCoreApplication::translate("fhmi", "信号格式错误"));
    }
    CanSignalDef parsed;
    parsed.name = match.captured(1);
    *mux = match.captured(2);
    parsed.start = match.captured(3).toInt();
    parsed.length = match.captured(4).toInt();
    parsed.motorola = match.captured(5) == QLatin1String("0");
    parsed.isSigned = match.captured(6) == QLatin1String("-");
    const auto number = [&match](int group, double fallback, bool *ok) {
        if (match.captured(group).isNull()) {
            return fallback;
        }
        bool good = false;
        const double value = match.captured(group).toDouble(&good);
        *ok = *ok && good && std::isfinite(value);
        return value;
    };
    bool ok = true;
    parsed.factor = number(7, 1, &ok);
    parsed.offset = number(8, 0, &ok);
    parsed.minimum = number(9, 0, &ok);
    parsed.maximum = number(10, 0, &ok);
    if (!ok || parsed.factor == 0) {
        return fail(error, QCoreApplication::translate("fhmi", "信号 %1 的因子、偏移或范围不是数字").arg(parsed.name));
    }
    parsed.unit = match.captured(11);
    *rest = match.captured(12).trimmed();
    if (!parsed.validate(error)) {
        return false;
    }
    *signal = parsed;
    return true;
}

std::optional<QList<InlineSignal>> parseInlineSignals(const QString &text, QString *error)
{
    QList<InlineSignal> out;
    const QStringList lines = text.split(QLatin1Char('\n'));
    static const QRegularExpression head(QStringLiteral("^(\\S+)\\s+(?:SG_\\s+)?(.*)$"));
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1String("//"))) {
            continue;
        }
        const auto bad = [&](const QString &why) {
            fail(error, QCoreApplication::translate("fhmi", "CAN 信号第 %1 行: %2").arg(i + 1).arg(why));
            return std::nullopt;
        };
        const QRegularExpressionMatch match = head.match(line);
        if (!match.hasMatch()) {
            return bad(QCoreApplication::translate("fhmi", "信号格式错误"));
        }
        QString why;
        const auto id = Can::parseId(match.captured(1), &why);
        if (!id) {
            return bad(why);
        }
        InlineSignal entry{*id, {}};
        QString mux;
        QString rest;
        if (!parseSignalBody(match.captured(2), &entry.signal, &mux, &rest, &why)) {
            return bad(why);
        }
        if (!mux.isEmpty() || !rest.isEmpty()) {
            return bad(QCoreApplication::translate("fhmi", "信号格式错误"));
        }
        out.append(entry);
    }
    return out;
}

std::optional<QList<QPair<QString, QString>>> parseAssignments(const QString &text, QString *error)
{
    QList<QPair<QString, QString>> out;
    static const QRegularExpression separators(QStringLiteral("[,;\\n]"));
    for (const QString &part : text.split(separators)) {
        const QString item = part.trimmed();
        if (item.isEmpty()) {
            continue;
        }
        const qsizetype equals = item.indexOf(QLatin1Char('='));
        const QString name = equals < 0 ? QString() : item.left(equals).trimmed();
        const QString value = equals < 0 ? QString() : item.mid(equals + 1).trimmed();
        if (name.isEmpty() || value.isEmpty()) {
            fail(error, QCoreApplication::translate("fhmi", "CAN 命令应写成 信号=值，用逗号分隔: %1").arg(item));
            return std::nullopt;
        }
        out.append({name, value});
    }
    if (out.isEmpty()) {
        fail(error, QCoreApplication::translate("fhmi", "CAN 命令应写成 信号=值，用逗号分隔: %1").arg(text));
        return std::nullopt;
    }
    return out;
}

// ---------------------------------------------------------------------------
// CanSignalSet
// ---------------------------------------------------------------------------

std::optional<CanSignalSet> CanSignalSet::build(std::shared_ptr<const CanDatabase> dbc, const QString &inlineText,
                                                QString *error)
{
    const auto lines = parseInlineSignals(inlineText, error);
    if (!lines) {
        return std::nullopt;
    }
    CanSignalSet set;
    set.m_dbc = std::move(dbc);
    QSet<QString> names;
    for (const InlineSignal &line : *lines) {
        const auto twice = [&]() {
            fail(error, QCoreApplication::translate("fhmi", "CAN 信号 %1 定义了两次").arg(line.signal.name));
            return std::nullopt;
        };
        if (names.contains(line.signal.name)) {
            return twice();
        }
        names.insert(line.signal.name);
        if (const CanMessageDef *message = set.m_dbc ? set.m_dbc->find(line.id) : nullptr) {
            for (const CanSignalDef &signal : message->signalDefs) {
                if (signal.name == line.signal.name) {
                    return twice();
                }
            }
        }
        auto it = set.m_inline.find(line.id.key());
        if (it == set.m_inline.end()) {
            set.m_inlineOrder.append(line.id);
            it = set.m_inline.insert(line.id.key(), {});
        }
        it->append(line.signal);
    }
    return set;
}

bool CanSignalSet::isEmpty() const
{
    return m_inline.isEmpty() && (!m_dbc || m_dbc->messages.isEmpty());
}

Message CanSignalSet::decode(const CanFrame &frame) const
{
    Message out;
    if (frame.error || frame.rtr) {
        return out;
    }
    const CanId id{frame.id, frame.extended};
    if (const CanMessageDef *message = m_dbc ? m_dbc->find(id) : nullptr) {
        const QString prefix = message->name + QLatin1Char('.');
        for (const CanSignalDef &signal : message->signalDefs) {
            const QVariant value = signal.decode(frame.data);
            if (!value.isValid()) {
                continue;
            }
            out.insert(prefix + signal.name, value);
            out.insert(signal.name, value);
            const QString text = signal.valueText(frame.data);
            if (!text.isEmpty()) {
                out.insert(prefix + signal.name + QLatin1String(".text"), text);
                out.insert(signal.name + QLatin1String(".text"), text);
            }
        }
    }
    const auto it = m_inline.constFind(id.key());
    if (it != m_inline.constEnd()) {
        for (const CanSignalDef &signal : *it) {
            const QVariant value = signal.decode(frame.data);
            if (value.isValid()) {
                out.insert(signal.name, value);
            }
        }
    }
    return out;
}

QString CanSignalSet::signalsText(const CanFrame &frame) const
{
    if (frame.error || frame.rtr) {
        return QString();
    }
    QStringList parts;
    const auto add = [&](const CanSignalDef &signal) {
        const QVariant value = signal.decode(frame.data);
        if (!value.isValid()) {
            return;
        }
        const QString text = signal.valueText(frame.data);
        QString shown = text.isEmpty() ? displayString(value) : text;
        if (text.isEmpty() && !signal.unit.isEmpty()) {
            shown += QLatin1Char(' ') + signal.unit;
        }
        parts << signal.name + QLatin1Char('=') + shown;
    };
    const CanId id{frame.id, frame.extended};
    if (const CanMessageDef *message = m_dbc ? m_dbc->find(id) : nullptr) {
        for (const CanSignalDef &signal : message->signalDefs) {
            add(signal);
        }
    }
    for (const CanSignalDef &signal : m_inline.value(id.key())) {
        add(signal);
    }
    return parts.join(QStringLiteral(", "));
}

QString CanSignalSet::messageName(const CanId &id) const
{
    const CanMessageDef *message = m_dbc ? m_dbc->find(id) : nullptr;
    return message ? message->name : QString();
}

std::optional<CanSignalSet::Target> CanSignalSet::target(const QString &canId, QString *error) const
{
    const QString text = canId.trimmed();
    Target target;
    const CanMessageDef *message = m_dbc ? m_dbc->findByName(text) : nullptr;
    if (message) {
        target.id = message->id;
    } else {
        QString why;
        const auto id = Can::parseId(text, &why);
        if (!id) {
            fail(error, m_dbc ? QCoreApplication::translate("fhmi", "DBC 中没有报文 %1，它也不是有效的 CAN ID").arg(text)
                              : why);
            return std::nullopt;
        }
        target.id = *id;
        message = m_dbc ? m_dbc->find(*id) : nullptr;
    }
    int needed = 0;
    if (message) {
        target.name = message->name;
        target.fd = message->fd;
        target.length = message->length;
        target.cycleTime = message->cycleTime;
        for (const CanSignalDef &signal : message->signalDefs) {
            target.signalDefs.append(&signal);
        }
    }
    const auto it = m_inline.constFind(target.id.key());
    if (it != m_inline.constEnd()) {
        for (const CanSignalDef &signal : *it) {
            target.signalDefs.append(&signal);
            needed = std::max(needed, signal.bytesNeeded());
        }
    }
    target.hasSignals = !target.signalDefs.isEmpty();
    if (!message && target.hasSignals) {
        // Inline signals only: a classic frame of 8 bytes unless they reach further, which takes an FD frame.
        target.fd = needed > 8;
        target.length = target.fd ? Can::fdPaddedLength(needed) : 8;
    }
    return target;
}

QStringList CanSignalSet::messageNames() const
{
    QStringList out;
    if (m_dbc) {
        for (const CanMessageDef &message : m_dbc->messages) {
            out << message.name;
        }
    }
    return out;
}

QStringList CanSignalSet::signalKeys() const
{
    QStringList out;
    QSet<QString> seen;
    const auto add = [&](const QString &key) {
        if (!seen.contains(key)) {
            seen.insert(key);
            out << key;
        }
    };
    if (m_dbc) {
        for (const CanMessageDef &message : m_dbc->messages) {
            for (const CanSignalDef &signal : message.signalDefs) {
                add(message.name + QLatin1Char('.') + signal.name);
                add(signal.name);
                if (!signal.valueDescriptions.isEmpty()) {
                    add(message.name + QLatin1Char('.') + signal.name + QLatin1String(".text"));
                    add(signal.name + QLatin1String(".text"));
                }
            }
        }
    }
    for (const CanId &id : m_inlineOrder) {
        for (const CanSignalDef &signal : m_inline.value(id.key())) {
            add(signal.name);
        }
    }
    return out;
}

QStringList CanSignalSet::warnings() const
{
    return m_dbc ? m_dbc->warnings : QStringList();
}

} // namespace fhmi
