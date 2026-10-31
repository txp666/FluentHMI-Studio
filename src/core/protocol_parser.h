#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <QtCore/QHash>

#include <memory>
#include <optional>
#include <utility>

namespace fhmi {

// One decoded message: field name -> value (qlonglong, double, QString, bool, or nested
// QVariantMap / QVariantList for JSON lines).
using Message = QVariantMap;

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
    virtual QList<Message> parse(const QByteArray &unit) = 0;
    // parse() of every unit frames() returns, in arrival order.
    QList<Message> feed(const QByteArray &data);
    // The units the buffer holds once the line has gone quiet; the buffer is empty afterwards.
    virtual QList<QByteArray> flush() { return {}; }
    // Milliseconds of silence after which the link calls flush(); 0 = never.
    virtual int idleTimeout() const { return 0; }
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

// Text records: JSON values, key=value pairs, or bare CSV (named ch0, ch1, ...). A record ends
// at the terminator; a JSON object or array ends where its brackets close, so pretty-printed
// JSON and objects sent back to back need no terminator. A record that starts with { or [ but
// holds a byte JSON cannot (outside a string) is dropped up to its terminator.
class LineParser : public Parser
{
  public:
    explicit LineParser(LineFrameConfig config = {}, int maxBuffer = 65536);

    // Buffers by itself: a unit is the bytes as read.
    QList<Message> parse(const QByteArray &data) override;
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

enum class HexChecksum
{
    None,
    Sum,         // the low 8 bits of the byte sum
    Xor,         // all bytes XORed
    Crc8,        // polynomial 0x07, initial 0x00, not reflected, no final XOR
    Crc16Modbus, // reflected 0x8005 (0xA001), initial 0xFFFF
    Crc16Ccitt,  // CRC-16/CCITT-FALSE: 0x1021, initial 0xFFFF, not reflected
};

// Bytes the checksum takes on the wire: 0, 1 or 2.
int hexChecksumSize(HexChecksum kind);
quint16 hexChecksum(HexChecksum kind, const QByteArray &data);

// The spellings .uidp files use for these settings, in the order the designer lists them.
namespace HexOptions {
const QStringList &checksums();    // 无, 和校验, 异或, CRC8, CRC16-Modbus, CRC16-CCITT
const QStringList &byteOrders();   // 大端(BE), 小端(LE)
const QStringList &lengthCovers(); // 数据, 长度字段之后, 整帧
const QStringList &fieldTypes();
} // namespace HexOptions

// Every key of a "hex" parserConfig. The defaults keep projects that only set header, footer
// and fields framing exactly as they did.
struct HexFrameConfig
{
    enum class LengthCovers
    {
        Data,       // the length counts the data after the length field, not checksum or footer
        AfterField, // everything after the length field
        Frame,      // the whole frame, header included
    };

    QByteArray header = QByteArray::fromHex("AA55"); // empty: no header
    QByteArray footer = QByteArray::fromHex("55AA"); // empty: no footer
    int lengthOffset = -1;                           // from the frame's first byte; -1 = no length field
    int lengthSize = 1;                              // 1 or 2
    bool lengthLittleEndian = false;
    LengthCovers lengthCovers = LengthCovers::Data;
    int fixedLength = 0; // the whole frame; 0 = not fixed
    HexChecksum checksum = HexChecksum::None;
    int checksumStart = 0; // checked from this frame offset up to the checksum bytes
    bool checksumLittleEndian = false;
    int idleTimeout = 0; // ms
    QList<HexField> fields;
};

// Binary frames: a header to synchronise on, then a fixed length, a length field or a footer
// to end the frame (or a silence, when none is set), optionally checksummed. frames() returns
// whole frames, header to footer; parse() reads the fields of one.
class HexFrameParser : public Parser
{
  public:
    explicit HexFrameParser(HexFrameConfig config);

    QList<QByteArray> frames(const QByteArray &data) override;
    QList<Message> parse(const QByteArray &frame) override;
    QList<QByteArray> flush() override;
    int idleTimeout() const override { return m_config.idleTimeout; }
    void reset() override { m_buf.clear(); }

  private:
    static constexpr int MaxFrame = 65536;

    // The length of the frame m_buf starts with: > 0 once known, 0 when these bytes cannot
    // start a frame, -1 while more bytes are needed.
    int frameLength() const;
    // Whether a frame has room for its header, checksum and footer, ends in the footer and
    // passes its checksum.
    bool valid(const QByteArray &frame) const;

    HexFrameConfig m_config;
    QByteArray m_buf;
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
    // {address, count} of the read this response answers. `answered` is the calling parser's
    // record of the RTU reads it has answered; with it, a read names one reply per parser.
    std::optional<std::pair<int, int>> take(int slave, int fc, int transaction = -1,
                                            QHash<int, int> *answered = nullptr) const;

  private:
    struct Read
    {
        int slave, fc, address, count;
    };
    struct Latest
    {
        int address, count, number; // number: which note this was, so a parser can tell it from one it answered
    };
    QHash<int, Latest> m_latest; // (slave << 8 | fc) -> the latest RTU read
    int m_noted = 0;
    QHash<int, Read> m_transactions;
    QList<int> m_order; // transactions, oldest first
};

// Modbus RTU responses, CRC-checked, resynchronising on noise.
class ModbusRtuParser : public Parser
{
  public:
    explicit ModbusRtuParser(QStringList fields = {}, std::shared_ptr<ModbusRequests> requests = nullptr);

    QList<Message> parse(const QByteArray &data) override;
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

    QList<Message> parse(const QByteArray &data) override;
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

    QList<Message> parse(const QByteArray &data) override;
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

} // namespace fhmi
