#include "comm/can/slcan_protocol.h"

namespace fhmi::Slcan {

namespace {

bool hexDigits(const QByteArray &text)
{
    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace

int bitrateCode(quint32 bitrate)
{
    static constexpr quint32 rates[] = {10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000};
    for (int i = 0; i < int(std::size(rates)); ++i) {
        if (rates[i] == bitrate) {
            return i;
        }
    }
    return -1;
}

std::optional<QByteArray> encodeFrame(const CanFrame &frame)
{
    if (frame.fd || frame.error || frame.data.size() > 8 || frame.id > (frame.extended ? Can::MaxExtendedId
                                                                                        : Can::MaxStandardId)) {
        return std::nullopt;
    }
    QByteArray line;
    line += frame.rtr ? (frame.extended ? 'R' : 'r') : (frame.extended ? 'T' : 't');
    line += QByteArray::number(frame.id, 16).toUpper().rightJustified(frame.extended ? 8 : 3, '0');
    line += char('0' + frame.data.size());
    if (!frame.rtr) {
        line += frame.data.toHex().toUpper();
    }
    line += '\r';
    return line;
}

std::optional<CanFrame> decodeFrame(const QByteArray &line)
{
    if (line.isEmpty()) {
        return std::nullopt;
    }
    const char kind = line.at(0);
    if (kind != 't' && kind != 'T' && kind != 'r' && kind != 'R') {
        return std::nullopt;
    }
    CanFrame frame;
    frame.extended = kind == 'T' || kind == 'R';
    frame.rtr = kind == 'r' || kind == 'R';
    const int idDigits = frame.extended ? 8 : 3;
    if (line.size() < 1 + idDigits + 1) {
        return std::nullopt;
    }
    const QByteArray id = line.mid(1, idDigits);
    const char lengthDigit = line.at(1 + idDigits);
    if (!hexDigits(id) || lengthDigit < '0' || lengthDigit > '8') {
        return std::nullopt;
    }
    frame.id = id.toUInt(nullptr, 16);
    if (frame.id > (frame.extended ? Can::MaxExtendedId : Can::MaxStandardId)) {
        return std::nullopt;
    }
    const int length = lengthDigit - '0';
    const qsizetype dataAt = 1 + idDigits + 1;
    const qsizetype dataDigits = frame.rtr ? 0 : 2 * length;
    const qsizetype rest = line.size() - dataAt - dataDigits;
    if (rest != 0 && rest != 4) { // nothing, or the time stamp
        return std::nullopt;
    }
    const QByteArray data = line.mid(dataAt, dataDigits);
    if (!hexDigits(data) || !hexDigits(line.mid(dataAt + dataDigits))) {
        return std::nullopt;
    }
    frame.data = frame.rtr ? QByteArray(length, '\0') : QByteArray::fromHex(data);
    return frame;
}

std::optional<quint8> decodeStatus(const QByteArray &line)
{
    if (line.size() != 3 || line.at(0) != 'F' || !hexDigits(line.mid(1))) {
        return std::nullopt;
    }
    return quint8(line.mid(1).toUInt(nullptr, 16));
}

} // namespace fhmi::Slcan
