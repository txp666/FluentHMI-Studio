#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QMetaType>
#include <QtCore/QString>

#include <optional>

namespace fhmi {

// One CAN or CAN FD frame, as every adapter backend reports it and every sender builds it.
struct CanFrame
{
    quint32 id = 0;
    bool extended = false;
    bool fd = false;
    bool brs = false;   // FD only: the data phase at the data bitrate
    bool rtr = false;   // classic remote frame: data.size() is the length asked for, its bytes unused
    bool error = false; // an error frame the adapter reported; never decoded
    QByteArray data;
    qint64 timestampMs = 0; // ms since the epoch, when it arrived or was sent
};

// An identifier with its format: 0x123 standard and 0x123 extended are different frames.
struct CanId
{
    quint32 id = 0;
    bool extended = false;

    bool operator==(const CanId &other) const { return id == other.id && extended == other.extended; }
    bool operator!=(const CanId &other) const { return !(*this == other); }
    // A key for hash tables: the extended flag above the 29 identifier bits.
    quint64 key() const { return (quint64(extended) << 32) | id; }
};

namespace Can {

constexpr quint32 MaxStandardId = 0x7FF;
constexpr quint32 MaxExtendedId = 0x1FFFFFFF;

// An ID as users write it everywhere (canId, canSignals, canFilter, DataSendCard): hexadecimal, optional 0x,
// any case. It is extended when its value is above 0x7FF or it is written with exactly 8 hex digits, as cansend
// reads 00000123 as extended 0x123.
std::optional<CanId> parseId(QStringView text, QString *error = nullptr);
// 0x0A8 for a standard ID, 0x18FF50E5 for an extended one.
QString idText(quint32 id, bool extended);
inline QString idText(const CanId &id) { return idText(id.id, id.extended); }

// The data lengths a CAN FD frame can have: 0-8, 12, 16, 20, 24, 32, 48, 64.
bool isValidFdLength(int length);
// The valid FD length a payload of `length` bytes is padded to; -1 when it is longer than 64.
int fdPaddedLength(int length);
// Pads an FD payload with 0x00 to the next valid length; false when it is longer than 64 bytes.
bool padFdPayload(QByteArray &data);

// A CAN FD data length code and the length it stands for (ISO 11898-1): 0-8 as they are, then 12, 16, 20, 24, 32,
// 48, 64. lengthToDlc takes a valid FD length (isValidFdLength).
int dlcToLength(int dlc);
int lengthToDlc(int length);

// The line a frame is shown as in DataRecvCard and the log: <ID> [FD] [BRS] [EXT] [RTR] [<len>] <HEX bytes>,
// e.g. "0x102 [8] 75 40 B8 0B 00 00 00 00".
QString frameLine(const CanFrame &frame);

// DataSendCard on a CAN link, cansend syntax: <ID>#<data> a classic frame, <ID>#R a remote frame (optionally
// #R<len>), <ID>##<flags><data> an FD frame whose flags nibble has BRS at bit 0. Data is hex bytes; dots and
// spaces between them are allowed. nullopt for anything else.
std::optional<CanFrame> parseSendText(const QString &text, QString *error = nullptr);

} // namespace Can

// The card's canFilter: empty passes everything, otherwise a list of IDs and ranges separated by commas or
// spaces, e.g. "100-1FF, 18FF50E5". A range bound is a single ID per Can::parseId; a range is extended when
// either bound is, and matches IDs of that format only.
class CanFilter
{
  public:
    static std::optional<CanFilter> parse(const QString &text, QString *error = nullptr);

    bool isEmpty() const { return m_ranges.isEmpty(); }
    bool accepts(quint32 id, bool extended) const;

  private:
    struct Range
    {
        quint32 first = 0;
        quint32 last = 0;
        bool extended = false;
    };
    QList<Range> m_ranges;
};

} // namespace fhmi

Q_DECLARE_METATYPE(fhmi::CanFrame)
