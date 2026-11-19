#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <QtCore/QHash>
#include <QtCore/QSet>

#include "core/hex_utils.h"
#include "core/message.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace fhmi {

namespace LinkParser {
inline const QString Inherit = QStringLiteral("项目设置"); // i18n: value
inline const QString HexGroup = QStringLiteral("hex_group");
inline const QString GroupTemplates = QStringLiteral("templates");

inline bool isHex(QStringView type)
{
    return type == QLatin1String("hex") || type == HexGroup;
}

inline bool inheritsProject(QStringView selection)
{
    return selection.isEmpty() || selection == Inherit;
}

inline QString effectiveType(const QString &selection, const QString &projectType)
{
    return inheritsProject(selection) ? projectType : selection;
}

struct Format
{
    QString type;
    QJsonObject settings;
};

inline Format effectiveFormat(const QString &selection, const QJsonObject &settings, const Format &project)
{
    if (inheritsProject(selection) || (settings.isEmpty() && selection == project.type))
        return project;
    return {selection, settings};
}

inline bool hasEditableFraming(QStringView type)
{
    return type == QLatin1String("line") || type == QLatin1String("hex");
}

inline bool supportsModbusSlave(QStringView type)
{
    return type == QLatin1String("modbus") || type == QLatin1String("modbus_tcp");
}
} // namespace LinkParser

struct FrameNotice
{
    enum class Reason { Length, Timeout, Checksum, Footer };
    Reason reason;
    QByteArray sample;
    qsizetype received;
    int expected;
};

struct FrameProgress
{
    qsizetype received = 0;
    int expected = 0;
};

struct DecodedRecord
{
    Message values;
    qsizetype end; // bytes through the end of this record in the current input
};

// Incremental frame parsers, in two steps. frames() cuts the bytes a link read into complete
// raw units (keeping partial input for the next call); parse() turns one unit into messages.
// Links publish each unit as raw bytes (HEX byte bindings, HEX curves, logs) and then its
// messages, so a binary frame reaches both whole. A parser that frames on silence asks for
// flush() after idleTimeout() ms without new bytes.
class Parser
{
  public:
    virtual ~Parser() = default;
    // The complete raw units in these bytes; by default the bytes as they came.
    virtual QList<QByteArray> frames(const QByteArray &data) { return {data}; }
    virtual QList<DecodedRecord> decodeRecords(const QByteArray &unit) = 0;
    QList<Message> parse(const QByteArray &unit);
    // parse() of every unit frames() returns, in arrival order.
    QList<Message> feed(const QByteArray &data);
    // The units the buffer holds once the line has gone quiet; the buffer is empty afterwards.
    virtual QList<QByteArray> flush() { return {}; }
    // Milliseconds of silence after which the link calls flush(); 0 = never.
    virtual int idleTimeout() const { return 0; }
    virtual bool flushOnMessage() const { return false; }
    virtual std::optional<FrameNotice> takeFrameNotice() { return {}; }
    virtual FrameProgress frameProgress() const { return {}; }
    virtual void reset() = 0;
};

// Frames raw widget bindings when both delimiters are configured. Without both, the read
// chunk stays the input unit: one widget's byte offset cannot imply a packet length.
class RawFrameBuffer
{
  public:
    explicit RawFrameBuffer(QByteArray header = {}, QByteArray footer = {}, int maxBuffer = 65536);

    QList<QByteArray> feed(const QByteArray &data);
    void reset() { m_buf.clear(); }

  private:
    QByteArray m_header;
    QByteArray m_footer;
    int m_maxBuffer;
    QByteArray m_buf;
};

// Every key of a "line" parserConfig.
struct LineFrameConfig
{
    QString separator = QStringLiteral(",");    // between the fields of a record
    QString terminator = QStringLiteral("\n");  // ends a record; a JSON value needs none
    QString kvSeparator = QStringLiteral("=");  // "=" or ":", the only one a record is split at
    QString encoding = QStringLiteral("utf-8"); // "utf-8", "gbk" or "ascii"
};

// The spellings .uidp files use for these settings, in the order the designer lists them.
namespace LineOptions {
const QStringList &kvSeparators(); // =, :
const QStringList &encodings();    // utf-8, gbk, ascii
} // namespace LineOptions

// The bytes a line parser built from this parserConfig ends its records with: the terminator in
// the record encoding (UTF-8 for "ascii", which cannot spell anything else).
QByteArray lineTerminator(const QJsonObject &config);
std::optional<QByteArray> buildLineFrame(const QString &body, const QJsonObject &config, QString *error = nullptr);

// Text records: JSON values, key=value pairs, or bare CSV (named ch0, ch1, ...). A record ends
// at the terminator; a JSON object or array ends where its brackets close, so pretty-printed
// JSON and objects sent back to back need no terminator. A record that starts with { or [ but
// holds a byte JSON cannot (outside a string) is dropped up to its terminator.
class LineParser : public Parser
{
  public:
    explicit LineParser(LineFrameConfig config = {}, int maxBuffer = 65536);

    // Buffers by itself: a unit is the bytes as read.
    QList<DecodedRecord> decodeRecords(const QByteArray &data) override;
    void reset() override;

    static QVariant autoNumber(const QString &text);
    // Bytes in one of the line encodings, each undecodable sequence replaced by U+FFFD the way
    // Python's errors="replace" does it, so both designers read a damaged record alike.
    static QString decode(const QByteArray &bytes, const QString &encoding);

  private:
    // A record, stripped and decoded; `raw` is its bytes, for an NMEA checksum.
    Message parseRecord(QString text, const QByteArray &raw) const;

    LineFrameConfig m_config;
    QByteArray m_terminator;
    int m_maxBuffer;
    QByteArray m_buf;
    bool m_discarding = false; // dropping an overlong or broken record, up to the next terminator
};

struct HexField
{
    QString name;
    int offset = 0; // from the first byte after the header
    int length = 0;
    QString type; // uint8, int8, uint16, int16, uint32, int32, float32, float64, ASCII, HEX
    bool littleEndian = false;
};

// Bytes the checksum takes on the wire: 0, 1 or 2.
int hexChecksumSize(HexChecksum kind);
quint16 hexChecksum(HexChecksum kind, const QByteArray &data);

// The spellings .uidp files use for these settings, in the order the designer lists them.
namespace HexOptions {
inline constexpr int LengthRecoveryMs = 1000;
const QStringList &checksums();    // 无, 和校验, 异或, CRC8, CRC16-Modbus, CRC16-CCITT
const QStringList &byteOrders();   // 大端(BE), 小端(LE)
const QStringList &lengthCovers(); // 数据, 长度字段之后, 整帧
const QStringList &fieldTypes();
int fieldSize(const QString &type); // 0 for ASCII and HEX
} // namespace HexOptions

// Every key of a "hex" parserConfig. The defaults keep projects that only set header, footer
// and fields framing exactly as they did.
struct HexFrameConfig
{
    using LengthCovers = HexFrameLayout::LengthCovers;

    QByteArray header = QByteArray::fromHex("AA55"); // empty: no header
    QByteArray footer = QByteArray::fromHex("55AA"); // empty: no footer
    QByteArray matchBytes;                         // optional discriminator in addition to the header
    int matchOffset = 0;                            // from the first byte of the frame
    int lengthOffset = -1;                           // from the frame's first byte; -1 = no length field
    int lengthSize = 1;                              // 1 or 2
    bool lengthLittleEndian = false;
    LengthCovers lengthCovers = LengthCovers::Data;
    int fixedLength = 0; // the whole frame; 0 = not fixed
    int maxFrameLength = 65536;
    HexChecksum checksum = HexChecksum::None;
    int checksumStart = 0; // checked from this frame offset up to the checksum bytes
    bool checksumLittleEndian = false;
    int idleTimeout = 0; // ms
    QList<HexField> fields;

    HexFrameLayout layout() const;
    bool hasBoundary() const { return fixedLength > 0 || lengthOffset >= 0 || !footer.isEmpty(); }
    qsizetype identificationEnd() const
    {
        return std::max(header.size(), matchBytes.isEmpty() ? qsizetype(0) : matchOffset + matchBytes.size());
    }
};

std::optional<HexFrameConfig> hexFrameConfig(const QJsonObject &config, QString *error = nullptr);

enum class HexFrameMatch { No, Partial, Yes };
HexFrameMatch matchHexFrame(QByteArrayView sample, const HexFrameConfig &config);

struct HexFrameVariant
{
    QString id;
    QString name;
    HexFrameConfig frame;
    QSet<QString> unqualifiedFields;
};

std::optional<QList<HexFrameVariant>> hexFrameVariants(const QJsonObject &settings, QString *error = nullptr);
QString hexTemplateFieldKey(const QString &templateId, const QString &name);

// Compose application bytes using the same length and checksum conventions as reception.
// A configured length field is inserted at its offset from the start of the frame.
std::optional<QByteArray> buildHexFrame(const QByteArray &body, const QByteArray &header, const QByteArray &footer,
                                      const QJsonObject &settings, QString *error = nullptr,
                                      HexFrameLayout *layout = nullptr);
std::optional<QByteArray> buildHexFrame(const QByteArray &body, const QJsonObject &settings,
                                      QString *error = nullptr, HexFrameLayout *layout = nullptr);

// Binary frames: a header to synchronise on, then a fixed length, a length field or a footer
// to end the frame (or a silence, when none is set), optionally checksummed. frames() returns
// whole frames, header to footer; parse() reads the fields of one.
class HexFrameParser : public Parser
{
  public:
    enum class Issue { None, Header, Identifier, Length, Footer, ChecksumRange, Checksum };
    explicit HexFrameParser(HexFrameConfig config);

    QList<QByteArray> frames(const QByteArray &data) override;
    QList<DecodedRecord> decodeRecords(const QByteArray &frame) override;
    QList<QByteArray> flush() override;
    int idleTimeout() const override { return m_buffer.idleMs; }
    bool flushOnMessage() const override { return m_buffer.flushOnMessage; }
    std::optional<FrameNotice> takeFrameNotice() override { return std::exchange(m_buffer.notice, {}); }
    FrameProgress frameProgress() const override { return m_buffer.progress(); }
    void reset() override { m_buffer = {}; }
    int declaredLength(QByteArrayView buffer) const;
    Issue sampleIssue(const QByteArray &frame, QByteArray *calculatedChecksum = nullptr) const;

  private:
    friend class HexFrameGroupParser;
    struct Buffer
    {
        QByteArray bytes;
        int expected = 0;
        int idleMs = 0;
        bool flushOnMessage = false;
        std::optional<FrameNotice> notice;
        FrameProgress progress() const { return expected ? FrameProgress{bytes.size(), expected} : FrameProgress{}; }
    };
    // A partial match carries the timeout of the candidate header/discriminator, even before the length is known.
    using Matcher = std::function<QPair<const HexFrameParser *, std::optional<int>>(QByteArrayView)>;
    static QList<QByteArray> scan(Buffer &buffer, const Matcher &match, bool expired);
    int incompleteTimeout() const;
    bool validLength(int length) const;
    QList<QByteArray> scan(bool expired);
    Issue trailerIssue(const QByteArray &frame, QByteArray *calculatedChecksum = nullptr) const;

    HexFrameConfig m_config;
    Buffer m_buffer;
};

// Read requests recently sent over one link, so a response can be named by register address.
// A Modbus response does not repeat the start address. Modbus TCP echoes the request's
// transaction id; an RTU master waits for each reply, so an RTU response answers the latest
// read with the same slave and function code. Each parser answers such a read once, so every
// client of a server names its reply and a later reply is not named by a stale read.
class ModbusRequests
{
  public:
    // A read of `count` registers or coils from `address`; transaction < 0 for RTU and ASCII.
    void note(int slave, int fc, int address, int count, int transaction = -1);
    // The read in an outgoing request frame, framed the way the link parses replies.
    void noteFrame(const QByteArray &frame, const QString &parserType);
    // Late TCP replies to timed-out transactions must not update the current data.
    void expireTcpRequest(const QByteArray &frame);
    bool expiredTcpReply(int transaction) const;
    // {address, count} of the read this response answers. `answered` is the calling parser's
    // record of the RTU reads it has answered; with it, a read names one reply per parser.
    std::optional<std::pair<int, int>> take(int slave, int fc, int transaction = -1,
                                            QHash<int, int> *answered = nullptr) const;

  private:
    struct Read
    {
        int slave, fc, address, count;
        bool expired = false;
    };
    struct Latest
    {
        int address, count, number; // number: which note this was, so a parser can tell it from one it answered
    };
    QHash<int, Latest> m_latest; // (slave << 8 | fc) -> the latest RTU read
    int m_noted = 0;
    QHash<int, Read> m_transactions;
    QList<int> m_order; // transactions, oldest first
    void rememberTcp(int transaction, const Read &read);
};

// Modbus RTU responses, CRC-checked, resynchronising on noise.
class ModbusRtuParser : public Parser
{
  public:
    explicit ModbusRtuParser(QStringList fields = {}, std::shared_ptr<ModbusRequests> requests = nullptr);

    QList<DecodedRecord> decodeRecords(const QByteArray &data) override;
    void reset() override { m_buf.clear(); }

    static int frameLength(const QByteArray &buf, int from = 0);

  private:
    QStringList m_fields;
    std::shared_ptr<ModbusRequests> m_requests;
    QHash<int, int> m_answered; // the reads this stream has answered (ModbusRequests::take)
    QByteArray m_buf;
};

// Modbus TCP response ADUs (MBAP header). Output matches ModbusRtuParser.
class ModbusTcpParser : public Parser
{
  public:
    explicit ModbusTcpParser(QStringList fields = {}, std::shared_ptr<ModbusRequests> requests = nullptr);

    QList<DecodedRecord> decodeRecords(const QByteArray &data) override;
    void reset() override { m_buf.clear(); }

    static int frameLength(const QByteArray &buf);

  private:
    QStringList m_fields;
    std::shared_ptr<ModbusRequests> m_requests;
    QByteArray m_buf;
};

// Modbus ASCII responses: ':', the RTU frame's bytes as hex digit pairs with an LRC in place of
// the CRC, then CR LF. A frame is what an RTU frame may be, and the output matches
// ModbusRtuParser, naming by address included: an ASCII master waits for each reply too.
class ModbusAsciiParser : public Parser
{
  public:
    explicit ModbusAsciiParser(QStringList fields = {}, std::shared_ptr<ModbusRequests> requests = nullptr);

    QList<DecodedRecord> decodeRecords(const QByteArray &data) override;
    void reset() override { m_buf.clear(); }

  private:
    QStringList m_fields;
    std::shared_ptr<ModbusRequests> m_requests;
    QHash<int, int> m_answered; // the reads this stream has answered (ModbusRequests::take)
    QByteArray m_buf;
};

// The response PDU body after the function code, as the shared reg/coil dictionary. When the
// read it answers is known (`request` is its address and count), "start" and keys by address
// follow: hr100 (holding register; FC 23 reads them too), ir100 (input register), co100 (coil),
// di100 (discrete input). FC 7 gives "status", FC 8 "subfunction" and "data", FC 17 "id", "run"
// and "extra", FC 22 "address", "andMask" and "orMask".
Message modbusResponse(int slave, int fc, const QByteArray &data, const QStringList &fields = {},
                       const std::optional<std::pair<int, int>> &request = std::nullopt);

QByteArray modbusCrc(const QByteArray &data);
// Modbus ASCII's check: the two's complement of the byte sum, low 8 bits.
quint8 modbusLrc(const QByteArray &data);
// ':' + `body` (slave, function code, data) and its LRC as uppercase hex + CR LF.
QByteArray modbusAsciiFrame(const QByteArray &body);
// The body of one whole ASCII frame, LRC checked and removed; nullopt for anything else.
std::optional<QByteArray> modbusAsciiBody(const QByteArray &frame);
// `registers` are the values an FC 16 request writes from `address` on.
QByteArray buildModbusRtuFrame(int slave, int func, int address, int value = 0, int count = 1,
                               const QList<quint16> &registers = {});
QByteArray buildModbusTcpFrame(int unit, int func, int address, int value = 0, int count = 1, int transaction = 0,
                               const QList<quint16> &registers = {});
QByteArray buildModbusAsciiFrame(int slave, int func, int address, int value = 0, int count = 1,
                                 const QList<quint16> &registers = {});

// Build a parser from the project's "parser" / "parserConfig" settings. Modbus parsers of one
// link share that link's `requests`. Returns nullptr and sets *error for an unknown type or an
// invalid configuration.
std::unique_ptr<Parser> createParser(const QString &type, const QJsonObject &config, QString *error = nullptr,
                                     std::shared_ptr<ModbusRequests> requests = nullptr);
// Whether a parser type reads Modbus replies: modbus, modbus_tcp or modbus_ascii.
bool isModbusParser(const QString &type);

} // namespace fhmi
