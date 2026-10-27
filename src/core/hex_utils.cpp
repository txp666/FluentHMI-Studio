#include "core/hex_utils.h"

namespace fhmi {

namespace {

int hexDigit(QChar c)
{
    const ushort u = c.unicode();
    if (u >= '0' && u <= '9') {
        return u - '0';
    }
    if (u >= 'a' && u <= 'f') {
        return u - 'a' + 10;
    }
    if (u >= 'A' && u <= 'F') {
        return u - 'A' + 10;
    }
    return -1;
}

} // namespace

std::optional<QByteArray> parseHex(const QString &text)
{
    QString compact;
    compact.reserve(text.size());
    for (const QChar c : text) {
        if (!c.isSpace()) {
            compact.append(c);
        }
    }
    if (compact.size() % 2) {
        compact.prepend(QLatin1Char('0'));
    }
    QByteArray out;
    out.reserve(compact.size() / 2);
    for (int i = 0; i < compact.size(); i += 2) {
        const int hi = hexDigit(compact.at(i));
        const int lo = hexDigit(compact.at(i + 1));
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        out.append(char((hi << 4) | lo));
    }
    return out;
}

QByteArray parseHexOrEmpty(const QString &text)
{
    return parseHex(text).value_or(QByteArray());
}

QString toHexString(const QByteArray &bytes, bool spaced)
{
    return QString::fromLatin1(spaced ? bytes.toHex(' ').toUpper() : bytes.toHex().toUpper());
}

} // namespace fhmi
