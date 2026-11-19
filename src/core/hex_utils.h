#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <optional>

namespace fhmi {

// Parse hex text such as "AA 55 0d0a". Whitespace is ignored and an odd number of
// digits gets a leading zero, matching the Python designer. Returns nullopt when
// any character is not a hex digit.
std::optional<QByteArray> parseHex(const QString &text);

// Like parseHex, but an invalid field yields an empty array (binding frame delimiters
// are optional, and a malformed one simply disables framing).
QByteArray parseHexOrEmpty(const QString &text);

QString toHexString(const QByteArray &bytes, bool spaced = true);

enum class HexChecksum
{
    None,
    Sum,         // the low 8 bits of the byte sum
    Xor,         // all bytes XORed
    Crc8,        // polynomial 0x07, initial 0x00, not reflected, no final XOR
    Crc16Modbus, // reflected 0x8005 (0xA001), initial 0xFFFF
    Crc16Ccitt,  // CRC-16/CCITT-FALSE: 0x1021, initial 0xFFFF, not reflected
};

struct FrameByteRange
{
    qsizetype offset;
    qsizetype size;
};

struct HexFrameLayout
{
    enum class LengthCovers
    {
        Data,       // Bytes after the length field, excluding the checksum and footer.
        AfterField, // Everything after the length field.
        Frame,      // The whole frame, including the length field itself.
    };

    qsizetype headerSize = 0;
    qsizetype footerSize = 0;
    qsizetype checksumSize = 0;
    qsizetype lengthOffset = -1;
    qsizetype lengthSize = 0;
    qsizetype checksumStart = 0;
    HexChecksum checksum = HexChecksum::None;
    LengthCovers lengthCovers = LengthCovers::Data;

    qsizetype checksumOffset(qsizetype frameSize) const { return frameSize - footerSize - checksumSize; }
    qsizetype lengthBase() const;
    std::optional<FrameByteRange> lengthCoverage(qsizetype frameSize) const;
    std::optional<FrameByteRange> checksumCoverage(qsizetype frameSize) const;

  private:
    qsizetype lengthCoverageStart() const;
};

} // namespace fhmi
