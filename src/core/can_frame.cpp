#include "core/can_frame.h"

#include "core/hex_utils.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>

#include <algorithm>

namespace fhmi {

namespace {

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool isHexDigit(QChar c)
{
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}

// Hex bytes with optional dots or spaces between them, as cansend writes them: whole bytes only.
std::optional<QByteArray> sendData(QStringView text)
{
    QString digits;
    for (const QChar c : text) {
        if (c == u'.' || c.isSpace()) {
            continue;
        }
        if (!isHexDigit(c)) {
            return std::nullopt;
        }
        digits += c;
    }
    if (digits.size() % 2 != 0) {
        return std::nullopt;
    }
    return QByteArray::fromHex(digits.toLatin1());
}

} // namespace

namespace Can {

std::optional<CanId> parseId(QStringView text, QString *error)
{
    QStringView digits = text.trimmed();
    const auto invalid = [&]() {
        fail(error, QCoreApplication::translate("fhmi", "CAN ID 无效: %1（十六进制，如 102 或 18FF50E5）")
                        .arg(text.trimmed().toString()));
        return std::nullopt;
    };
    if (digits.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        digits = digits.mid(2);
    }
    if (digits.isEmpty() || digits.size() > 8) {
        return invalid();
    }
    for (const QChar c : digits) {
        if (!isHexDigit(c)) {
            return invalid();
        }
    }
    bool ok = false;
    const quint32 value = digits.toUInt(&ok, 16);
    if (!ok || value > MaxExtendedId) {
        return invalid();
    }
    return CanId{value, value > MaxStandardId || digits.size() == 8};
}

QString idText(quint32 id, bool extended)
{
    return QStringLiteral("0x") + QStringLiteral("%1").arg(id, extended ? 8 : 3, 16, QLatin1Char('0')).toUpper();
}

bool isValidFdLength(int length)
{
    return (length >= 0 && length <= 8) || length == 12 || length == 16 || length == 20 || length == 24 ||
           length == 32 || length == 48 || length == 64;
}

int fdPaddedLength(int length)
{
    static constexpr int lengths[] = {12, 16, 20, 24, 32, 48, 64};
    if (length < 0) {
        return -1;
    }
    if (length <= 8) {
        return length;
    }
    for (const int valid : lengths) {
        if (length <= valid) {
            return valid;
        }
    }
    return -1;
}

bool padFdPayload(QByteArray &data)
{
    const int padded = fdPaddedLength(int(data.size()));
    if (padded < 0) {
        return false;
    }
    data.append(QByteArray(padded - data.size(), '\0'));
    return true;
}

int dlcToLength(int dlc)
{
    static constexpr int lengths[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return lengths[std::clamp(dlc, 0, 15)];
}

int lengthToDlc(int length)
{
    for (int dlc = 0; dlc < 16; ++dlc) {
        if (dlcToLength(dlc) >= length) {
            return dlc;
        }
    }
    return 15;
}

QString frameLine(const CanFrame &frame)
{
    QStringList parts{idText(frame.id, frame.extended)};
    if (frame.error) {
        parts << QStringLiteral("[ERR]");
    }
    if (frame.fd) {
        parts << QStringLiteral("[FD]");
    }
    if (frame.brs) {
        parts << QStringLiteral("[BRS]");
    }
    if (frame.extended) {
        parts << QStringLiteral("[EXT]");
    }
    if (frame.rtr) {
        parts << QStringLiteral("[RTR]");
    }
    parts << QStringLiteral("[%1]").arg(frame.data.size());
    if (!frame.data.isEmpty() && !frame.rtr) {
        parts << toHexString(frame.data);
    }
    return parts.join(QLatin1Char(' '));
}

std::optional<CanFrame> parseSendText(const QString &text, QString *error)
{
    const QString input = text.trimmed();
    const auto invalid = [error]() {
        fail(error, QCoreApplication::translate("fhmi", "CAN 链路请按 ID#数据 格式输入"));
        return std::nullopt;
    };
    const qsizetype hash = input.indexOf(QLatin1Char('#'));
    if (hash <= 0) {
        return invalid();
    }
    const auto id = parseId(QStringView(input).left(hash), error);
    if (!id) {
        return std::nullopt;
    }
    CanFrame frame;
    frame.id = id->id;
    frame.extended = id->extended;
    QStringView rest = QStringView(input).mid(hash + 1);
    if (rest.startsWith(u'#')) {
        // ##<flags><data>: one hex nibble of flags, then the payload.
        if (rest.size() < 2 || !isHexDigit(rest.at(1))) {
            return invalid();
        }
        const int flags = QString(rest.at(1)).toInt(nullptr, 16);
        const auto data = sendData(rest.mid(2));
        if (!data) {
            return invalid();
        }
        frame.fd = true;
        frame.brs = flags & 0x1;
        frame.data = *data;
        if (!padFdPayload(frame.data)) {
            fail(error, QCoreApplication::translate("fhmi", "CAN FD 数据最多 64 字节"));
            return std::nullopt;
        }
        return frame;
    }
    if (rest.startsWith(u'R') || rest.startsWith(u'r')) {
        // A remote frame asks for `len` bytes; its data field stays empty.
        const QStringView length = rest.mid(1).trimmed();
        bool ok = length.isEmpty();
        const int requested = length.isEmpty() ? 0 : length.toInt(&ok);
        if (!ok || requested < 0 || requested > 8) {
            return invalid();
        }
        frame.rtr = true;
        frame.data = QByteArray(requested, '\0');
        return frame;
    }
    const auto data = sendData(rest);
    if (!data) {
        return invalid();
    }
    if (data->size() > 8) {
        fail(error, QCoreApplication::translate("fhmi", "CAN 数据最多 8 字节，CAN FD 请用 ID##标志数据"));
        return std::nullopt;
    }
    frame.data = *data;
    return frame;
}

} // namespace Can

std::optional<CanFilter> CanFilter::parse(const QString &text, QString *error)
{
    CanFilter filter;
    QString normalized = text;
    normalized.replace(QStringLiteral("，"), QStringLiteral(",")); // i18n: ignore
    for (QString part : normalized.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts)) {
        const qsizetype dash = part.indexOf(QLatin1Char('-'));
        const auto first = Can::parseId(dash < 0 ? QStringView(part) : QStringView(part).left(dash), error);
        const auto last = dash < 0 ? first : Can::parseId(QStringView(part).mid(dash + 1), error);
        if (!first || !last) {
            return std::nullopt;
        }
        if (last->id < first->id) {
            fail(error, QCoreApplication::translate("fhmi", "CAN 过滤范围无效: %1").arg(part));
            return std::nullopt;
        }
        filter.m_ranges.append({first->id, last->id, first->extended || last->extended});
    }
    return filter;
}

bool CanFilter::accepts(quint32 id, bool extended) const
{
    if (m_ranges.isEmpty()) {
        return true;
    }
    for (const Range &range : m_ranges) {
        if (range.extended == extended && id >= range.first && id <= range.last) {
            return true;
        }
    }
    return false;
}

} // namespace fhmi
